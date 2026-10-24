#include "ir_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "io.h"
#include "storage_fs.h"

/* IR files live at the root of the shared LittleFS mount (STORAGE_FS_BASE),
 * so the on-flash layout is identical to the pre-shared-mount build
 * (base "/ir" also stored names at the partition root) — no data migration. */
#define IR_BASE_PATH  STORAGE_FS_BASE
#define IR_PART_LABEL "storage"
#define IR_EXT        ".ir"
#define IR_STORE_MAX_FILE_BYTES 8192

static bool s_ready = false;

bool ir_store_ready(void) { return s_ready; }

esp_err_t ir_store_init(void)
{
    if (s_ready) return ESP_OK;
    esp_err_t err = storage_fs_mount();   /* shared mount (see storage_fs.c) */
    if (err != ESP_OK) {
        io_log("ir: littlefs mount failed (%s)\r\n", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

/* Sanitize a user name into a safe basename (no extension). */
static void sanitize(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; in && in[i] && n + 1 < cap; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
            out[n++] = c;
        } else if (c == ' ') {
            out[n++] = '_';
        }
    }
    if (n == 0) { out[n++] = 's'; out[n++] = 'i'; out[n++] = 'g'; }
    out[n] = '\0';
}

static void path_for(const char *name, char *path, size_t cap)
{
    char safe[IR_NAME_MAX + 1];
    sanitize(name, safe, sizeof(safe));
    snprintf(path, cap, IR_BASE_PATH "/%s" IR_EXT, safe);
}

static bool append_format(char *buf, size_t cap, size_t *used,
                          const char *fmt, ...)
{
    if (*used >= cap) return false;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *used, cap - *used, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= cap - *used) return false;

    *used += (size_t)written;
    return true;
}

esp_err_t ir_store_save(const char *name,
                        const uint16_t *durations, size_t count,
                        uint32_t carrier_hz, float duty,
                        const ir_message_t *decoded)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name || !durations || count == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(name) > IR_NAME_MAX) return ESP_ERR_INVALID_SIZE;
    if (count > IR_RAW_MAX_TIMINGS) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; name[i]; i++) {
        if (iscntrl((unsigned char)name[i])) return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        if (durations[i] == 0) return ESP_ERR_INVALID_ARG;
    }
    if (carrier_hz == 0) carrier_hz = 38000;
    if (duty <= 0.0f || duty >= 1.0f) duty = 0.33f;

    char path[80];
    path_for(name, path, sizeof(path));

    /* Build the whole file in RAM, then write it in one pass — fewest
     * LittleFS/flash transactions (decoupled from the RX capture path). */
    size_t cap = 384 + strlen(name) + count * 6;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { io_log("ir: save oom\r\n"); return ESP_ERR_NO_MEM; }

    /* Format duty without %f (ESP-IDF nano printf may omit float). */
    int duty_whole = (int)duty;
    int duty_frac = (int)((duty - (float)duty_whole) * 1000000.0f + 0.5f);
    if (duty_frac >= 1000000) duty_frac = 999999;

    size_t used = 0;
    bool ok = append_format(buf, cap, &used,
                            "Filetype: IR signals file\nVersion: 1\n#\n");
    if (decoded && decoded->proto != IR_PROTO_UNKNOWN &&
        decoded->proto != IR_PROTO_RAW && ok) {
        ok = append_format(buf, cap, &used,
                           "# proto: %s\n# address: 0x%08lX\n# command: 0x%08lX\n",
                           ir_proto_name(decoded->proto),
                           (unsigned long)decoded->address,
                           (unsigned long)decoded->command);
    }
    if (ok) {
        ok = append_format(buf, cap, &used,
                           "name: %s\ntype: raw\nfrequency: %lu\nduty_cycle: %d.%06d\ndata:",
                           name, (unsigned long)carrier_hz,
                           duty_whole, duty_frac);
    }
    for (size_t i = 0; i < count && ok; i++) {
        ok = append_format(buf, cap, &used, " %u", (unsigned)durations[i]);
    }
    if (ok) ok = append_format(buf, cap, &used, "\n");
    if (!ok) {
        heap_caps_free(buf);
        io_log("ir: save buffer size invalid\r\n");
        return ESP_ERR_INVALID_SIZE;
    }

    char tmp_path[sizeof(path) + 4];
    int path_len = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (path_len < 0 || (size_t)path_len >= sizeof(tmp_path)) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        heap_caps_free(buf);
        io_log("ir: save open failed\r\n");
        return ESP_FAIL;
    }
    size_t wr = fwrite(buf, 1, used, f);
    int rc = fclose(f);
    heap_caps_free(buf);
    if (wr != used || rc != 0) {
        if (remove(tmp_path) != 0 && errno != ENOENT) {
            io_log("ir: save temporary cleanup failed\r\n");
        }
        io_log("ir: save write failed\r\n");
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        if (remove(tmp_path) != 0 && errno != ENOENT) {
            io_log("ir: save temporary cleanup failed\r\n");
        }
        io_log("ir: save replace failed\r\n");
        return ESP_FAIL;
    }
    io_log("ir: SAVED name=%s timings=%u\r\n", name, (unsigned)count);
    return ESP_OK;
}

esp_err_t ir_store_list(ir_store_list_cb cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    DIR *dir = opendir(IR_BASE_PATH);
    if (!dir) return ESP_FAIL;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *dn = de->d_name;
        size_t len = strlen(dn);
        size_t ext = strlen(IR_EXT);
        if (len <= ext || strcasecmp(dn + len - ext, IR_EXT) != 0) continue;

        char base[IR_NAME_MAX + 8];
        size_t blen = len - ext;
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, dn, blen);
        base[blen] = '\0';

        size_t sz = 0;
        /* d_name can be up to 255 bytes per dirent.h — size for the worst
         * case so -Wformat-truncation is satisfied. */
        char path[sizeof(IR_BASE_PATH) + 260];
        snprintf(path, sizeof(path), IR_BASE_PATH "/%s", dn);
        struct stat st;
        if (stat(path, &st) == 0) sz = (size_t)st.st_size;

        if (cb) cb(base, sz, ctx);
    }
    closedir(dir);
    return ESP_OK;
}

static void count_cb(const char *name, size_t sz, void *ctx)
{
    (void)name; (void)sz;
    (*(int *)ctx)++;
}

int ir_store_count(void)
{
    int n = 0;
    ir_store_list(count_cb, &n);
    return n;
}

/* Parse "k v" style lines + the "data:" timing list from a file buffer. */
static size_t parse_data(char *buf, uint16_t *out, size_t out_cap)
{
    char *p = strstr(buf, "data:");
    if (!p) return 0;
    p += 5;
    char *line_end = strchr(p, '\n');
    if (line_end) *line_end = '\0';
    size_t n = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(p, " \t\r", &saveptr);
    while (tok) {
        if (n >= out_cap) return 0;
        errno = 0;
        char *end = NULL;
        long v = strtol(tok, &end, 10);
        if (errno == ERANGE || end == tok || *end != '\0' ||
            v <= 0 || v > UINT16_MAX) {
            return 0;
        }
        out[n++] = (uint16_t)v;
        tok = strtok_r(NULL, " \t\r", &saveptr);
    }
    return n;
}

static bool find_u32(const char *buf, const char *key, uint32_t *out)
{
    const char *p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || *p == '\0') return false;

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(p, &end, 0);
    if (errno == ERANGE || end == p || value > UINT32_MAX) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0' && *end != '\r' && *end != '\n') return false;
    *out = (uint32_t)value;
    return true;
}

esp_err_t ir_store_load(const char *name,
                        uint16_t *out, size_t out_cap, size_t *out_count,
                        uint32_t *carrier_hz, float *duty,
                        ir_message_t *decoded)
{
    if (out_count) *out_count = 0;
    if (carrier_hz) *carrier_hz = 38000;
    if (duty) *duty = 0.33f;
    if (decoded) {
        memset(decoded, 0, sizeof(*decoded));
        decoded->proto = IR_PROTO_UNKNOWN;
    }
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name || !out || out_cap == 0) return ESP_ERR_INVALID_ARG;

    char path[80];
    path_for(name, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t fsz = (size_t)st.st_size;
    if (fsz == 0 || fsz > IR_STORE_MAX_FILE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = heap_caps_malloc(fsz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    FILE *f = fopen(path, "r");
    if (!f) { heap_caps_free(buf); return ESP_FAIL; }
    size_t rd = fread(buf, 1, fsz, f);
    int read_error = ferror(f);
    int close_error = fclose(f);
    if (rd != fsz || read_error || close_error != 0) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    buf[rd] = '\0';

    uint32_t freq = 38000;
    if (strstr(buf, "frequency:") &&
        (!find_u32(buf, "frequency:", &freq) || freq == 0)) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    float d = 0.33f;
    const char *dp = strstr(buf, "duty_cycle:");
    if (dp) {
        const char *value_start = dp + 11;
        errno = 0;
        char *end = NULL;
        d = strtof(value_start, &end);
        while (*end == ' ' || *end == '\t') end++;
        if (errno == ERANGE || end == value_start ||
            (*end != '\0' && *end != '\r' && *end != '\n') ||
            !isfinite(d) || d <= 0.0f || d >= 1.0f) {
            heap_caps_free(buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (decoded) {
        memset(decoded, 0, sizeof(*decoded));
        decoded->proto = IR_PROTO_RAW;
        /* Our own metadata comments, if present. */
        const char *pp = strstr(buf, "# proto:");
        if (pp) {
            char pn[24] = {0};
            sscanf(pp + 8, " %23s", pn);
            ir_proto_t pr = ir_proto_from_name(pn);
            if (pr != IR_PROTO_UNKNOWN) decoded->proto = pr;
        }
        if ((strstr(buf, "# address:") &&
             !find_u32(buf, "# address:", &decoded->address)) ||
            (strstr(buf, "# command:") &&
             !find_u32(buf, "# command:", &decoded->command))) {
            heap_caps_free(buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    size_t n = parse_data(buf, out, out_cap);
    heap_caps_free(buf);
    if (n == 0) return ESP_FAIL;

    if (out_count) *out_count = n;
    if (carrier_hz) *carrier_hz = freq;
    if (duty) *duty = (d > 0.0f && d < 1.0f) ? d : 0.33f;
    return ESP_OK;
}

esp_err_t ir_store_delete(const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[80];
    path_for(name, path, sizeof(path));
    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;
    io_log("ir: deleted name=%s\r\n", name);
    return ESP_OK;
}

esp_err_t ir_store_usage(size_t *total, size_t *used)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return esp_littlefs_info(IR_PART_LABEL, total, used);
}
