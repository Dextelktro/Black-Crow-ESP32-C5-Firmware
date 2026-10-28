/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subghz_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "nvs.h"

#include "io.h"
#include "sd_storage.h"
#include "storage_fs.h"

#define SUBGHZ_LFS_DIR     STORAGE_FS_BASE "/subghz"   /* "/fs/subghz"      */
#define SUBGHZ_SD_DIR      "/sdcard/subghz"
#define SUBGHZ_EXT         ".sub"
#define SUBGHZ_RAW_PER_LINE 512                        /* values per RAW_Data line */
#define SUBGHZ_NVS_NS      "subghz"
#define SUBGHZ_NVS_KEY_TGT "store_tgt"
#define SUBGHZ_PATH_MAX    200
#define SUBGHZ_MAX_FILE    65536   /* refuse to load anything larger */
#define SUBGHZ_MAX_DURATION_US 1000000  /* reject absurd single durations (1 s) */

static subghz_store_target_t s_target = SUBGHZ_STORE_UNSET;

/* ---------------------------------------------------------------- */

esp_err_t subghz_store_init(void)
{
    nvs_handle_t h;
    if (nvs_open(SUBGHZ_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = SUBGHZ_STORE_UNSET;
        /* Only accept a valid stored value; anything else stays UNSET — never
         * silently fall back to a different target. */
        if (nvs_get_u8(h, SUBGHZ_NVS_KEY_TGT, &v) == ESP_OK &&
            (v == SUBGHZ_STORE_LITTLEFS || v == SUBGHZ_STORE_SD)) {
            s_target = (subghz_store_target_t)v;
        }
        nvs_close(h);
    }
    return ESP_OK;
}

subghz_store_target_t subghz_store_get_target(void) { return s_target; }

const char *subghz_store_target_name(subghz_store_target_t target)
{
    switch (target) {
        case SUBGHZ_STORE_LITTLEFS: return "littlefs";
        case SUBGHZ_STORE_SD:       return "sd";
        default:                    return "unset";
    }
}

esp_err_t subghz_store_set_target(subghz_store_target_t target)
{
    if (target != SUBGHZ_STORE_LITTLEFS && target != SUBGHZ_STORE_SD) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(SUBGHZ_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, SUBGHZ_NVS_KEY_TGT, (uint8_t)target);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    /* Only adopt the new target if it truly persisted — do not report a choice
     * as saved when the NVS write/commit failed. */
    if (err == ESP_OK) s_target = target;
    return err;
}

/* ---- target resolution (snapshot once per operation) ---- */

static const char *dir_for(subghz_store_target_t tgt)
{
    return tgt == SUBGHZ_STORE_SD ? SUBGHZ_SD_DIR : SUBGHZ_LFS_DIR;
}

/* Validate `tgt` is usable right now; fill *is_sd. UNSET -> INVALID_STATE,
 * SD-but-absent -> NOT_FOUND. */
static esp_err_t resolve_target(subghz_store_target_t tgt, bool *is_sd)
{
    if (tgt == SUBGHZ_STORE_SD) {
        bool present = false, mounted = false, busy = false, safe = false;
        sd_storage_get_state(&present, &mounted, &busy, &safe);
        if (!present || !mounted) return ESP_ERR_NOT_FOUND;
        *is_sd = true;
        return ESP_OK;
    }
    if (tgt == SUBGHZ_STORE_LITTLEFS) {
        esp_err_t err = storage_fs_mount();
        if (err != ESP_OK) return err;
        *is_sd = false;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;   /* UNSET */
}

bool subghz_store_target_available(void)
{
    bool is_sd = false;
    return resolve_target(s_target, &is_sd) == ESP_OK;
}

/* ---- name handling (untrusted input) ---- */

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
    out[n] = '\0';
}

/* Turn an untrusted user name into a safe basename (no extension). REJECTS
 * (returns false) rather than silently transforming: empty, too long, "."/"..",
 * or any path separator / control char — so a name can never escape the target
 * directory. A trailing ".sub" is stripped so we never produce "x.sub.sub". */
static bool prepare_basename(const char *name, char *out, size_t cap)
{
    if (!name || !*name) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strlen(name) > SUBGHZ_STORE_NAME_MAX) return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
    }

    char safe[SUBGHZ_STORE_NAME_MAX + 1];
    sanitize(name, safe, sizeof(safe));   /* defense in depth */
    size_t sl = strlen(safe);
    size_t el = strlen(SUBGHZ_EXT);
    if (sl > el && strcasecmp(safe + sl - el, SUBGHZ_EXT) == 0) {
        safe[sl - el] = '\0';             /* avoid x.sub.sub */
    }
    if (safe[0] == '\0') return false;

    int n = snprintf(out, cap, "%s", safe);
    return n > 0 && (size_t)n < cap;
}

/* ---- file content ---- */

static bool append_format(char *buf, size_t cap, size_t *used, const char *fmt, ...)
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

/* Strict base-10 unsigned parse of a header field value: the number must be
 * followed only by whitespace, end-of-line or end-of-string, so a trailing-
 * garbage field like "12x" or "390abc" is REJECTED rather than silently
 * truncated to 12 / 390. Returns false on no-digits, overflow or trailing junk. */
static bool parse_ul_strict(const char *p, unsigned long *out)
{
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0' && *end != '\r' && *end != '\n') return false;
    *out = v;
    return true;
}

static size_t build_sub(char *buf, size_t cap, const int32_t *durations,
                        size_t count, uint32_t freq_hz, const char *preset)
{
    size_t used = 0;
    bool ok = append_format(buf, cap, &used,
                            "Filetype: Flipper SubGhz RAW File\n"
                            "Version: 1\n"
                            "Frequency: %lu\n"
                            "Preset: %s\n"
                            "Protocol: RAW\n",
                            (unsigned long)freq_hz,
                            preset ? preset : "FuriHalSubGhzPresetOok650Async");
    for (size_t i = 0; i < count && ok; i++) {
        if (i % SUBGHZ_RAW_PER_LINE == 0) {
            ok = append_format(buf, cap, &used, "%sRAW_Data: ",
                               i == 0 ? "" : "\n");
        }
        if (ok) {
            ok = append_format(buf, cap, &used, "%s%ld",
                               (i % SUBGHZ_RAW_PER_LINE == 0) ? "" : " ",
                               (long)durations[i]);
        }
    }
    if (ok) ok = append_format(buf, cap, &used, "\n");
    return ok ? used : 0;
}

static void ensure_dir(bool is_sd)
{
    if (is_sd) {
        if (mkdir(SUBGHZ_SD_DIR, 0775) != 0 && errno != EEXIST) {
            io_log("subghz: sd mkdir subghz failed (%d)\r\n", errno);
        }
    } else {
        storage_fs_create_directory("subghz");  /* EEXIST is fine */
    }
}

/* Write buf to a sibling .tmp then rename to path. Caller guarantees `path`
 * does NOT already exist, so FAT's non-overwriting rename succeeds and an
 * existing file can never be destroyed by a partial write. Logs the specific
 * failure and returns ESP_FAIL. */
static esp_err_t write_atomic(const char *path, const char *buf, size_t len)
{
    char tmp[SUBGHZ_PATH_MAX + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(tmp, "w");
    if (!f) { io_log("subghz: ERR write-failed (open)\r\n"); return ESP_FAIL; }

    size_t wr = fwrite(buf, 1, len, f);
    int werrno = errno;
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);           /* best-effort durability before rename */
    int rc = fclose(f);

    if (wr != len || rc != 0) {
        remove(tmp);
        if (wr != len && werrno == ENOSPC) {
            io_log("subghz: ERR no-space\r\n");
        } else {
            io_log("subghz: ERR write-failed\r\n");
        }
        return ESP_FAIL;
    }
    if (rename(tmp, path) != 0) {
        io_log("subghz: ERR rename-failed (%d)\r\n", errno);
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- public ops ---- */

esp_err_t subghz_store_save_raw(const char *name, const int32_t *durations,
                                size_t count, uint32_t freq_hz,
                                const char *preset_flipper_name)
{
    if (!name || !durations || count == 0) return ESP_ERR_INVALID_ARG;
    if (count > SUBGHZ_STORE_MAX_TIMINGS) {
        io_log("subghz: ERR too-many-timings %u\r\n", (unsigned)count);
        return ESP_ERR_INVALID_SIZE;
    }

    char base[SUBGHZ_STORE_NAME_MAX + 1];
    if (!prepare_basename(name, base, sizeof(base))) {
        io_log("subghz: ERR bad-name\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    subghz_store_target_t tgt = s_target;   /* snapshot for the whole op */
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;          /* UNSET / SD-absent -> caller logs */

    char path[SUBGHZ_PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s" SUBGHZ_EXT, dir_for(tgt), base);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    /* Build the whole file in PSRAM. Budget the worst case per token, not the
     * typical one: a signed int32 duration is up to 11 chars ("-2147483648")
     * plus a separator, and every SUBGHZ_RAW_PER_LINE tokens adds a
     * "\nRAW_Data: " prefix — so 14 bytes/token with a fixed 512-byte header
     * slack covers any valid capture (count<=1024) and never fails a legitimate
     * save with a spurious INVALID_SIZE. */
    size_t cap = 512 + count * 14;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { io_log("subghz: ERR save-oom\r\n"); return ESP_ERR_NO_MEM; }
    size_t len = build_sub(buf, cap, durations, count, freq_hz,
                           preset_flipper_name);
    if (len == 0) { heap_caps_free(buf); return ESP_ERR_INVALID_SIZE; }

    if (is_sd) {
        err = sd_storage_acquire();
        if (err != ESP_OK) { heap_caps_free(buf); return err; }
    }
    ensure_dir(is_sd);

    /* No silent overwrite: refuse if the destination already exists, so an
     * existing file is never deleted/replaced without an explicit choice. */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (is_sd) sd_storage_release();
        heap_caps_free(buf);
        io_log("subghz: ERR exists name=%s\r\n", base);
        return ESP_FAIL;
    }

    err = write_atomic(path, buf, len);
    if (is_sd) sd_storage_release();
    heap_caps_free(buf);

    if (err == ESP_OK) {
        io_log("subghz: SAVED name=%s n=%u target=%s\r\n",
               base, (unsigned)count, subghz_store_target_name(tgt));
    }
    return err;
}

esp_err_t subghz_store_list(subghz_store_list_cb cb, void *ctx)
{
    subghz_store_target_t tgt = s_target;
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;

    if (is_sd) {
        err = sd_storage_acquire();
        if (err != ESP_OK) return err;
    }
    const char *dir = dir_for(tgt);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *dn = de->d_name;
            size_t len = strlen(dn);
            size_t ext = strlen(SUBGHZ_EXT);
            /* Only .sub files; the sibling ".sub.tmp" ends in .tmp and is
             * therefore never listed as a capture. */
            if (len <= ext || strcasecmp(dn + len - ext, SUBGHZ_EXT) != 0) {
                continue;
            }
            char base[SUBGHZ_STORE_NAME_MAX + 8];
            size_t blen = len - ext;
            if (blen >= sizeof(base)) blen = sizeof(base) - 1;
            memcpy(base, dn, blen);
            base[blen] = '\0';

            size_t sz = 0;
            char full[SUBGHZ_PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", dir, dn) > 0) {
                struct stat st;
                if (stat(full, &st) == 0) sz = (size_t)st.st_size;
            }
            if (cb) cb(base, sz, ctx);
        }
        closedir(d);
    }
    if (is_sd) sd_storage_release();
    return ESP_OK;   /* missing dir == empty library */
}

esp_err_t subghz_store_delete(const char *name)
{
    char base[SUBGHZ_STORE_NAME_MAX + 1];
    if (!prepare_basename(name, base, sizeof(base))) return ESP_ERR_INVALID_ARG;

    subghz_store_target_t tgt = s_target;
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;

    char path[SUBGHZ_PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s" SUBGHZ_EXT, dir_for(tgt), base);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    if (is_sd) {
        err = sd_storage_acquire();
        if (err != ESP_OK) return err;
    }
    int rc = remove(path);
    int rerrno = errno;
    if (is_sd) sd_storage_release();
    if (rc == 0) return ESP_OK;
    /* Distinguish a missing file from a real I/O error (e.g. SD glitch). */
    return rerrno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

esp_err_t subghz_store_load_raw(const char *name, int32_t *out, size_t cap,
                                size_t *out_count, uint32_t *out_freq,
                                char *preset_out, size_t preset_cap)
{
    if (out_count) *out_count = 0;
    if (out_freq) *out_freq = 0;
    if (preset_out && preset_cap) preset_out[0] = '\0';
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;

    char base[SUBGHZ_STORE_NAME_MAX + 1];
    if (!prepare_basename(name, base, sizeof(base))) return ESP_ERR_INVALID_ARG;

    subghz_store_target_t tgt = s_target;
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;

    char path[SUBGHZ_PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s" SUBGHZ_EXT, dir_for(tgt), base);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    /* All locals declared up front so the error `goto done` never jumps over an
     * initialized declaration (portable; avoids -Wjump-misses-init). */
    char *buf = NULL;
    FILE *f = NULL;
    size_t n = 0;
    struct stat st;
    size_t fsz = 0;
    size_t rd = 0;
    char *p = NULL;
    bool overflow = false;

    if (is_sd) {
        err = sd_storage_acquire();
        if (err != ESP_OK) return err;
    }

    if (stat(path, &st) != 0) { err = ESP_ERR_NOT_FOUND; goto done; }
    fsz = (size_t)st.st_size;
    if (fsz == 0 || fsz > SUBGHZ_MAX_FILE) { err = ESP_ERR_INVALID_SIZE; goto done; }

    buf = heap_caps_malloc(fsz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { err = ESP_ERR_NO_MEM; goto done; }

    f = fopen(path, "r");
    if (!f) { err = ESP_FAIL; goto done; }
    rd = fread(buf, 1, fsz, f);
    if (rd != fsz || ferror(f)) {
        fclose(f);
        f = NULL;
        err = ESP_FAIL;
        goto done;
    }
    fclose(f);
    f = NULL;
    buf[rd] = '\0';

    /* Header fields (before any RAW_Data). Frequency is mandatory and
     * overflow-checked — a RAW file without a usable frequency is rejected,
     * never defaulted. */
    if (out_freq) {
        const char *fp = strstr(buf, "Frequency:");
        if (!fp) { err = ESP_ERR_INVALID_RESPONSE; goto done; }
        errno = 0;
        char *fend = NULL;
        unsigned long fv = strtoul(fp + 10, &fend, 10);
        if (fend == fp + 10 || errno == ERANGE || fv == 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
        *out_freq = (uint32_t)fv;
    }
    if (preset_out && preset_cap) {
        const char *pp = strstr(buf, "Preset:");
        if (pp) {
            pp += 7;
            while (*pp == ' ' || *pp == '\t') pp++;
            size_t k = 0;
            while (*pp && *pp != '\r' && *pp != '\n' && *pp != ' ' &&
                   k + 1 < preset_cap) {
                preset_out[k++] = *pp++;
            }
            preset_out[k] = '\0';
        }
    }

    /* Every RAW_Data line (there may be several), signed us values. STRICT: a
     * malformed/zero/out-of-range token rejects the whole file rather than
     * being silently skipped (which would quietly alter the waveform); more
     * values than the buffer holds is rejected too, never truncated. */
    p = buf;
    while ((p = strstr(p, "RAW_Data:")) != NULL) {
        p += 9;  /* strlen("RAW_Data:") */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(p, " \t\r", &save); tok;
             tok = strtok_r(NULL, " \t\r", &save)) {
            errno = 0;
            char *end = NULL;
            long v = strtol(tok, &end, 10);
            int64_t mag = v;
            if (mag < 0) mag = -mag;            /* int64: safe for INT32_MIN */
            if (end == tok || *end != '\0' || errno == ERANGE ||
                v == 0 || mag > SUBGHZ_MAX_DURATION_US) {
                if (nl) *nl = '\n';
                err = ESP_ERR_INVALID_RESPONSE;   /* strict reject */
                goto done;
            }
            if (n >= cap) { overflow = true; break; }
            out[n++] = (int32_t)v;
        }
        if (nl) *nl = '\n';
        if (overflow || !nl) break;
        p = nl + 1;
    }

    if (overflow) { err = ESP_ERR_INVALID_SIZE; goto done; }  /* too many timings */
    if (n < 2) { err = ESP_ERR_INVALID_RESPONSE; goto done; }
    if (out_count) *out_count = n;
    err = ESP_OK;

done:
    if (f) fclose(f);
    if (buf) heap_caps_free(buf);
    if (is_sd) sd_storage_release();
    return err;
}

/* Read a stored .sub whole into a NUL-terminated PSRAM buffer (caller frees).
 * Handles name sanitize, target resolve, and SD acquire/release. */
static esp_err_t load_file_psram(const char *name, char **out_buf)
{
    *out_buf = NULL;
    char base[SUBGHZ_STORE_NAME_MAX + 1];
    if (!prepare_basename(name, base, sizeof(base))) return ESP_ERR_INVALID_ARG;
    subghz_store_target_t tgt = s_target;
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;
    char path[SUBGHZ_PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s" SUBGHZ_EXT, dir_for(tgt), base);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    char *buf = NULL;
    FILE *f = NULL;
    size_t fsz = 0;
    struct stat st;

    if (is_sd) { err = sd_storage_acquire(); if (err != ESP_OK) return err; }
    if (stat(path, &st) != 0) { err = ESP_ERR_NOT_FOUND; goto done; }
    fsz = (size_t)st.st_size;
    if (fsz == 0 || fsz > SUBGHZ_MAX_FILE) { err = ESP_ERR_INVALID_SIZE; goto done; }
    buf = heap_caps_malloc(fsz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { err = ESP_ERR_NO_MEM; goto done; }
    f = fopen(path, "r");
    if (!f) { err = ESP_FAIL; goto done; }
    if (fread(buf, 1, fsz, f) != fsz || ferror(f)) { err = ESP_FAIL; goto done; }
    buf[fsz] = '\0';
    *out_buf = buf; buf = NULL; err = ESP_OK;
done:
    if (f) fclose(f);
    if (buf) heap_caps_free(buf);
    if (is_sd) sd_storage_release();
    return err;
}

subghz_file_kind_t subghz_store_classify(const char *name)
{
    char *buf = NULL;
    if (load_file_psram(name, &buf) != ESP_OK || !buf) return SUBGHZ_FILE_NONE;
    subghz_file_kind_t kind = SUBGHZ_FILE_NONE;
    const char *pp = strstr(buf, "Protocol:");
    if (pp) {
        pp += 9;
        while (*pp == ' ' || *pp == '\t') pp++;
        if (strncasecmp(pp, "RAW", 3) == 0 &&
            (pp[3] == '\0' || pp[3] == '\r' || pp[3] == '\n' || pp[3] == ' ')) {
            kind = SUBGHZ_FILE_RAW;
        } else {
            kind = SUBGHZ_FILE_KEY;
        }
    }
    heap_caps_free(buf);
    return kind;
}

static size_t build_key_file(char *buf, size_t cap, const char *proto,
                             uint16_t bits, uint64_t key, uint32_t te,
                             uint32_t freq, const char *preset)
{
    size_t used = 0;
    bool ok = append_format(buf, cap, &used,
                            "Filetype: Flipper SubGhz Key File\nVersion: 1\n"
                            "Frequency: %lu\nPreset: %s\nProtocol: %s\nBit: %u\n",
                            (unsigned long)freq,
                            preset ? preset : "FuriHalSubGhzPresetOok650Async",
                            proto ? proto : "Unknown", (unsigned)bits);
    if (ok) ok = append_format(buf, cap, &used, "Key:");
    for (int b = 7; b >= 0 && ok; b--) {
        ok = append_format(buf, cap, &used, " %02lX",
                           (unsigned long)((key >> (b * 8)) & 0xFF));
    }
    if (ok) ok = append_format(buf, cap, &used, "\nTE: %lu\n", (unsigned long)te);
    return ok ? used : 0;
}

esp_err_t subghz_store_save_key(const char *name, const char *proto_name,
                                uint16_t bits, uint64_t key, uint32_t te,
                                uint32_t freq_hz, const char *preset_flipper_name)
{
    if (!name || !proto_name || bits == 0 || bits > 64) return ESP_ERR_INVALID_ARG;
    char base[SUBGHZ_STORE_NAME_MAX + 1];
    if (!prepare_basename(name, base, sizeof(base))) {
        io_log("subghz: ERR bad-name\r\n");
        return ESP_ERR_INVALID_ARG;
    }
    subghz_store_target_t tgt = s_target;
    bool is_sd = false;
    esp_err_t err = resolve_target(tgt, &is_sd);
    if (err != ESP_OK) return err;

    char path[SUBGHZ_PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s" SUBGHZ_EXT, dir_for(tgt), base);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    char buf[256];
    size_t len = build_key_file(buf, sizeof(buf), proto_name, bits, key, te,
                                freq_hz, preset_flipper_name);
    if (len == 0) return ESP_ERR_INVALID_SIZE;

    if (is_sd) { err = sd_storage_acquire(); if (err != ESP_OK) return err; }
    ensure_dir(is_sd);
    struct stat st;
    if (stat(path, &st) == 0) {
        if (is_sd) sd_storage_release();
        io_log("subghz: ERR exists name=%s\r\n", base);
        return ESP_FAIL;
    }
    err = write_atomic(path, buf, len);
    if (is_sd) sd_storage_release();
    if (err == ESP_OK) {
        io_log("subghz: SAVED name=%s proto=%s bit=%u target=%s\r\n",
               base, proto_name, (unsigned)bits, subghz_store_target_name(tgt));
    }
    return err;
}

esp_err_t subghz_store_load_key(const char *name,
                                char *proto_out, size_t proto_cap,
                                uint16_t *bits, uint64_t *key, uint32_t *te,
                                uint32_t *freq, char *preset_out, size_t preset_cap)
{
    if (proto_out && proto_cap) proto_out[0] = '\0';
    if (preset_out && preset_cap) preset_out[0] = '\0';
    if (bits) *bits = 0;
    if (key) *key = 0;
    if (te) *te = 0;
    if (freq) *freq = 0;

    char *buf = NULL;
    esp_err_t err = load_file_psram(name, &buf);
    if (err != ESP_OK) return err;
    err = ESP_ERR_INVALID_RESPONSE;

    /* Frequency (mandatory). */
    const char *fp = strstr(buf, "Frequency:");
    if (!fp) goto done;
    unsigned long fv = 0;
    if (!parse_ul_strict(fp + 10, &fv) || fv == 0) goto done;
    if (freq) *freq = (uint32_t)fv;

    /* Protocol (mandatory, a token). */
    const char *pp = strstr(buf, "Protocol:");
    if (!pp) goto done;
    pp += 9;
    while (*pp == ' ' || *pp == '\t') pp++;
    if (proto_out && proto_cap) {
        size_t k = 0;
        while (*pp && *pp != '\r' && *pp != '\n' && *pp != ' ' && k + 1 < proto_cap) {
            proto_out[k++] = *pp++;
        }
        proto_out[k] = '\0';
    }

    /* Bit (mandatory). */
    const char *bp = strstr(buf, "Bit:");
    if (!bp) goto done;
    unsigned long bv = 0;
    if (!parse_ul_strict(bp + 4, &bv) || bv == 0 || bv > 64) goto done;
    if (bits) *bits = (uint16_t)bv;

    /* Key: up to 8 space-separated hex bytes, MSB first. */
    char *kp = strstr(buf, "Key:");
    if (!kp) goto done;
    kp += 4;
    char *nl = strchr(kp, '\n');
    if (nl) *nl = '\0';
    uint64_t kacc = 0;
    int nb = 0;
    char *save = NULL;
    for (char *tok = strtok_r(kp, " \t\r", &save); tok && nb < 8;
         tok = strtok_r(NULL, " \t\r", &save)) {
        char *end = NULL;
        long b = strtol(tok, &end, 16);
        if (end == tok || *end != '\0' || b < 0 || b > 0xFF) { nb = 0; break; }
        kacc = (kacc << 8) | (uint64_t)(b & 0xFF);
        nb++;
    }
    if (nl) *nl = '\n';
    if (nb == 0) goto done;
    if (key) *key = kacc;

    /* Preset (optional) + TE (optional). */
    if (preset_out && preset_cap) {
        const char *rp = strstr(buf, "Preset:");
        if (rp) {
            rp += 7;
            while (*rp == ' ' || *rp == '\t') rp++;
            size_t k = 0;
            while (*rp && *rp != '\r' && *rp != '\n' && *rp != ' ' && k + 1 < preset_cap) {
                preset_out[k++] = *rp++;
            }
            preset_out[k] = '\0';
        }
    }
    /* TE (optional): absent is fine, but present-but-malformed REJECTS the file
     * rather than silently using a half-parsed value. */
    const char *tp = strstr(buf, "TE:");
    if (tp) {
        unsigned long tv = 0;
        if (!parse_ul_strict(tp + 3, &tv)) goto done;
        if (te) *te = (uint32_t)tv;
    }

    err = ESP_OK;
done:
    heap_caps_free(buf);
    return err;
}
