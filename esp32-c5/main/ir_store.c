#include "ir_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "io.h"
#include "storage_fs.h"

/* IR files live at the root of the shared LittleFS mount (STORAGE_FS_BASE),
 * so the on-flash layout is identical to the pre-shared-mount build
 * (base "/ir" also stored names at the partition root) — no data migration. */
#define IR_BASE_PATH  STORAGE_FS_BASE
#define IR_PART_LABEL "storage"
#define IR_EXT        ".ir"

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

esp_err_t ir_store_save(const char *name,
                        const uint16_t *durations, size_t count,
                        uint32_t carrier_hz, float duty,
                        const ir_message_t *decoded)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name || !durations || count == 0) return ESP_ERR_INVALID_ARG;
    if (count > IR_RAW_MAX_TIMINGS) count = IR_RAW_MAX_TIMINGS;
    if (carrier_hz == 0) carrier_hz = 38000;
    if (duty <= 0.0f || duty >= 1.0f) duty = 0.33f;

    char path[80];
    path_for(name, path, sizeof(path));

    /* Build the whole file in RAM, then write it in one pass — fewest
     * LittleFS/flash transactions (decoupled from the RX capture path). */
    size_t cap = 256 + count * 7;   /* up to 6 chars + space per timing */
    char *buf = malloc(cap);
    if (!buf) { io_log("ir: save oom\r\n"); return ESP_ERR_NO_MEM; }

    /* Format duty without %f (ESP-IDF nano printf may omit float). */
    int duty_whole = (int)duty;
    int duty_frac = (int)((duty - (float)duty_whole) * 1000000.0f + 0.5f);

    int p = 0;
    p += snprintf(buf + p, cap - p,
                  "Filetype: IR signals file\nVersion: 1\n#\n");
    if (decoded && decoded->proto != IR_PROTO_UNKNOWN &&
        decoded->proto != IR_PROTO_RAW) {
        p += snprintf(buf + p, cap - p,
                      "# proto: %s\n# address: 0x%08lX\n# command: 0x%08lX\n",
                      ir_proto_name(decoded->proto),
                      (unsigned long)decoded->address,
                      (unsigned long)decoded->command);
    }
    p += snprintf(buf + p, cap - p,
                  "name: %s\ntype: raw\nfrequency: %lu\nduty_cycle: %d.%06d\ndata:",
                  name, (unsigned long)carrier_hz, duty_whole, duty_frac);
    for (size_t i = 0; i < count && p < (int)cap - 8; i++) {
        p += snprintf(buf + p, cap - p, " %u", (unsigned)durations[i]);
    }
    p += snprintf(buf + p, cap - p, "\n");

    FILE *f = fopen(path, "w");
    if (!f) {
        free(buf);
        io_log("ir: save open failed\r\n");
        return ESP_FAIL;
    }
    size_t wr = fwrite(buf, 1, (size_t)p, f);
    int rc = fclose(f);
    free(buf);
    if (wr != (size_t)p || rc != 0) {
        io_log("ir: save write failed\r\n");
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
    char *tok = strtok(p, " \t");
    while (tok && n < out_cap) {
        long v = strtol(tok, NULL, 10);
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
        out[n++] = (uint16_t)v;
        tok = strtok(NULL, " \t");
    }
    return n;
}

static bool find_u32(const char *buf, const char *key, uint32_t *out)
{
    const char *p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    *out = (uint32_t)strtoul(p, NULL, 0);
    return true;
}

esp_err_t ir_store_load(const char *name,
                        uint16_t *out, size_t out_cap, size_t *out_count,
                        uint32_t *carrier_hz, float *duty,
                        ir_message_t *decoded)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name || !out || out_cap == 0) return ESP_ERR_INVALID_ARG;

    char path[80];
    path_for(name, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t fsz = (size_t)st.st_size;
    char *buf = malloc(fsz + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    FILE *f = fopen(path, "r");
    if (!f) { free(buf); return ESP_FAIL; }
    size_t rd = fread(buf, 1, fsz, f);
    fclose(f);
    buf[rd] = '\0';

    uint32_t freq = 38000;
    find_u32(buf, "frequency:", &freq);
    float d = 0.33f;
    const char *dp = strstr(buf, "duty_cycle:");
    if (dp) d = strtof(dp + 11, NULL);

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
        find_u32(buf, "# address:", &decoded->address);
        find_u32(buf, "# command:", &decoded->command);
    }

    size_t n = parse_data(buf, out, out_cap);
    free(buf);
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
