/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "acl.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "io.h"

#define NVS_NS    "deauther"
#define NVS_KEY_W "wl"
#define NVS_KEY_B "bl"

typedef struct {
    uint8_t    mac[6];
    uint8_t    kind;   /* acl_kind_t */
    uint8_t    used;
} acl_entry_t;

typedef struct {
    acl_entry_t entries[ACL_MAX];
} acl_list_t;

static acl_list_t s_wl, s_bl;
static SemaphoreHandle_t s_mutex;

#define LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mutex)

static int find(acl_list_t *l, const uint8_t *mac)
{
    for (int i = 0; i < ACL_MAX; i++) {
        if (l->entries[i].used && memcmp(l->entries[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static int find_free(acl_list_t *l)
{
    for (int i = 0; i < ACL_MAX; i++) {
        if (!l->entries[i].used) return i;
    }
    return -1;
}

static int count(const acl_list_t *l)
{
    int c = 0;
    for (int i = 0; i < ACL_MAX; i++) if (l->entries[i].used) c++;
    return c;
}

static esp_err_t persist(const char *key, const acl_list_t *l)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, l, sizeof(*l));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load(const char *key, acl_list_t *l)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        io_log("acl: failed to open stored %s (err=%d)\r\n", key, err);
        return err;
    }
    size_t sz = sizeof(*l);
    err = nvs_get_blob(h, key, l, &sz);
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        memset(l, 0, sizeof(*l));
        io_log("acl: failed to load stored %s (err=%d); using empty list\r\n",
               key, err);
        return err;
    }
    return ESP_OK;
}

esp_err_t acl_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    memset(&s_wl, 0, sizeof(s_wl));
    memset(&s_bl, 0, sizeof(s_bl));
    esp_err_t wl_err = load(NVS_KEY_W, &s_wl);
    esp_err_t bl_err = load(NVS_KEY_B, &s_bl);
    return wl_err != ESP_OK ? wl_err : bl_err;
}

static int add(acl_list_t *l, const uint8_t *mac, acl_kind_t kind)
{
    if (find(l, mac) >= 0) return -2;
    int i = find_free(l);
    if (i < 0) return -1;
    memcpy(l->entries[i].mac, mac, 6);
    l->entries[i].kind = (uint8_t)kind;
    l->entries[i].used = 1;
    return 0;
}

static int remove_mac(acl_list_t *l, const uint8_t *mac)
{
    int i = find(l, mac);
    if (i < 0) return -1;
    memset(&l->entries[i], 0, sizeof(l->entries[i]));
    return 0;
}

static void clear(acl_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

static void list(const acl_list_t *l, const char *tag)
{
    int n = 0;
    for (int i = 0; i < ACL_MAX; i++) {
        if (!l->entries[i].used) continue;
        const acl_entry_t *e = &l->entries[i];
        const char *k = (e->kind == ACL_KIND_BSSID) ? "bssid" :
                        (e->kind == ACL_KIND_STA)   ? "sta"   : "auto";
        io_log("%s  %02X:%02X:%02X:%02X:%02X:%02X  %s\r\n",
               tag, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], k);
        n++;
    }
    if (n == 0) io_log("(%s empty)\r\n", tag);
}

int acl_wl_add(const uint8_t *mac, acl_kind_t kind)
{
    LOCK();
    acl_list_t previous = s_wl;
    int rc = add(&s_wl, mac, kind);
    if (rc == 0) {
        esp_err_t err = persist(NVS_KEY_W, &s_wl);
        if (err != ESP_OK) {
            s_wl = previous;
            io_log("acl: failed to persist whitelist (err=%d); change reverted\r\n",
                   err);
            rc = -3;
        }
    }
    UNLOCK();
    return rc;
}

int acl_wl_remove(const uint8_t *mac)
{
    LOCK();
    acl_list_t previous = s_wl;
    int rc = remove_mac(&s_wl, mac);
    if (rc == 0) {
        esp_err_t err = persist(NVS_KEY_W, &s_wl);
        if (err != ESP_OK) {
            s_wl = previous;
            io_log("acl: failed to persist whitelist (err=%d); change reverted\r\n",
                   err);
            rc = -2;
        }
    }
    UNLOCK();
    return rc;
}

int acl_wl_clear(void)
{
    LOCK();
    acl_list_t previous = s_wl;
    clear(&s_wl);
    esp_err_t err = persist(NVS_KEY_W, &s_wl);
    if (err != ESP_OK) {
        s_wl = previous;
        io_log("acl: failed to persist whitelist clear (err=%d); change reverted\r\n",
               err);
    }
    UNLOCK();
    return err == ESP_OK ? 0 : -1;
}

bool acl_wl_contains(const uint8_t *mac)
{
    LOCK();
    bool r = (find(&s_wl, mac) >= 0);
    UNLOCK();
    return r;
}

void acl_wl_list(void)
{
    LOCK();
    list(&s_wl, "wl");
    UNLOCK();
}

int acl_bl_add(const uint8_t *mac, acl_kind_t kind)
{
    LOCK();
    acl_list_t previous = s_bl;
    int rc = add(&s_bl, mac, kind);
    if (rc == 0) {
        esp_err_t err = persist(NVS_KEY_B, &s_bl);
        if (err != ESP_OK) {
            s_bl = previous;
            io_log("acl: failed to persist blacklist (err=%d); change reverted\r\n",
                   err);
            rc = -3;
        }
    }
    UNLOCK();
    return rc;
}

int acl_bl_remove(const uint8_t *mac)
{
    LOCK();
    acl_list_t previous = s_bl;
    int rc = remove_mac(&s_bl, mac);
    if (rc == 0) {
        esp_err_t err = persist(NVS_KEY_B, &s_bl);
        if (err != ESP_OK) {
            s_bl = previous;
            io_log("acl: failed to persist blacklist (err=%d); change reverted\r\n",
                   err);
            rc = -2;
        }
    }
    UNLOCK();
    return rc;
}

int acl_bl_clear(void)
{
    LOCK();
    acl_list_t previous = s_bl;
    clear(&s_bl);
    esp_err_t err = persist(NVS_KEY_B, &s_bl);
    if (err != ESP_OK) {
        s_bl = previous;
        io_log("acl: failed to persist blacklist clear (err=%d); change reverted\r\n",
               err);
    }
    UNLOCK();
    return err == ESP_OK ? 0 : -1;
}

bool acl_bl_contains(const uint8_t *mac)
{
    LOCK();
    bool r = (find(&s_bl, mac) >= 0);
    UNLOCK();
    return r;
}

void acl_bl_list(void)
{
    LOCK();
    list(&s_bl, "bl");
    UNLOCK();
}

int acl_bl_get(int idx, uint8_t *mac_out, acl_kind_t *kind_out)
{
    LOCK();
    int seen = 0;
    int rc = -1;
    for (int i = 0; i < ACL_MAX; i++) {
        if (!s_bl.entries[i].used) continue;
        if (seen == idx) {
            memcpy(mac_out, s_bl.entries[i].mac, 6);
            if (kind_out) *kind_out = (acl_kind_t)s_bl.entries[i].kind;
            rc = 0;
            break;
        }
        seen++;
    }
    UNLOCK();
    return rc;
}

int acl_bl_count(void)
{
    LOCK();
    int c = count(&s_bl);
    UNLOCK();
    return c;
}

int acl_parse_mac(const char *s, uint8_t *out)
{
    if (!s || !out) return -1;
    unsigned int a, b, c, d, e, f;
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &a, &b, &c, &d, &e, &f);
    if (n != 6) {
        n = sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x", &a, &b, &c, &d, &e, &f);
        if (n != 6) return -1;
    }
    out[0] = a; out[1] = b; out[2] = c; out[3] = d; out[4] = e; out[5] = f;
    return 0;
}
