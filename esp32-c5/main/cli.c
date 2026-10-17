#include "cli.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_system.h"

#include "io.h"
#include "wifi_ctrl.h"
#include "sniffer.h"
#include "targets.h"
#include "acl.h"
#include "attack.h"
#include "ble_spam.h"
#include "ble_scanner.h"
#include "ble_spectrum.h"
#include "ble_wardrive.h"
#include "nrf24.h"
#include "monitor.h"
#include "spectrum.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"
#include "evil_portal.h"
#include "led.h"
#include "ir.h"
#include "storage_fs.h"

#define CLI_LINE_MAX 256

static char s_line[CLI_LINE_MAX];
static size_t s_len = 0;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_command_queue;
static bool s_command_queue_overflow;

typedef struct {
    char line[CLI_LINE_MAX];
} cli_command_t;

#define CLI_COMMAND_QUEUE_LENGTH 8

static const char *HELP =
    "commands:\r\n"
    "  scan                            dual-band WiFi scan\r\n"
    "  scan stop | clear               stop scan / clear network results\r\n"
    "  ls                              list APs\r\n"
    "  sniff <ap_idx> <sec>            promiscuous capture for that AP\r\n"
    "  stas                            list captured STAs\r\n"
    "  dev scan [sec]                  passive device sweep (default 13s)\r\n"
    "  dev ls                          list found devices\r\n"
    "  t24 <ap_idx>                    select 2.4 GHz target\r\n"
    "  t5  <ap_idx>                    select 5 GHz target\r\n"
    "  sta <sta_idx>                   select unicast STA target\r\n"
    "  clear                           clear selected targets\r\n"
    "  sel <idx> | sel ls | sel clear  multi-AP selection\r\n"
    "  mode <broadcast|unicast|disassoc|authflood|mixed>\r\n"
    "  start <duration_sec>            start attack (selected APs)\r\n"
    "  nuke <duration_sec>             attack ALL scanned APs (mixed)\r\n"
    "  deauthall [--skip|--unicast] [sec]  channel-hop deauth\r\n"
    "     (default --skip: skip AP yang punya whitelisted STA)\r\n"
    "     (--unicast: downgrade ke unicast per STA non-WL — butuh sniff)\r\n"
    "  deauthall rescan                manual rescan during deauth all\r\n"
    "  da [--skip|--unicast] [sec]     alias deauthall\r\n"
    "  blespam <type> [delay] [sec]    BLE spam\r\n"
    "  blespam stop                    stop BLE spam\r\n"
    "  blespam delay <ms>              set BLE spam delay runtime\r\n"
    "  blescan start [active] [sec]    BLE device scan (default 30s)\r\n"
    "  blescan stop                    stop BLE scan\r\n"
    "  blescan ls|clear|status         scan results / reset / status\r\n"
    "  blewar start [active] [sec]     BLE wardriving (live, 0=forever)\r\n"
    "  blewar stop | status            control BLE wardriving\r\n"
    "  blefind save <address>          save an AirTag-format candidate to LittleFS\r\n"
    "  monitor <ap_idx>                live profile of one AP\r\n"
    "  monitor stop                    stop channel monitor\r\n"
    "  wlscan start <fast|balanced|deep> [itv_ms] [sec]  WiFi live scan\r\n"
    "  wlscan stop | status            control WiFi live scan\r\n"
    "  spectrum start [5] | stop       WiFi channel spectrum (5=incl 5GHz)\r\n"
    "  blespectrum start | stop        BLE signal spectrum\r\n"
    "  nrf scan                        2.4GHz RPD scan, report peak channel\r\n"
    "  nrf waterfall | nrf waterfall stop   rolling RPD sweep\r\n"
    "  nrf wfjam [sec]                 scan + jam strongest channel\r\n"
    "  nrf ls | status | stopall       scan table / status / stop all nRF\r\n"
    "  nrf jam auto <target> [sec]     jam preset (wifi|ble_adv|ble_data|...)\r\n"
    "  nrf jam manual <target> <strat> <rate> <pa> <dwell> <burst> [sec]\r\n"
    "  nrf jam stop                    stop jammer\r\n"
    "  nrf sniff start <ch> [sec] | stop | ls | clear\r\n"
    "  nrf replay <idx> [count] [delay_us]\r\n"
    "  nrf mj scan [sec] | stop | ls | clear | inject <idx> <text>\r\n"
    "  wspam start <mode> [rate] [band] [sec]  WiFi beacon spam\r\n"
    "     mode: funny|rickroll|random|all|custom  rate: aggressive|normal|stealth\r\n"
    "     band: 2g|dual   sec: duration (0=infinite)\r\n"
    "  wspam stop | status | add <ssid> | clear | ls\r\n"
    "  portal start <template> [ssid]  evil captive portal (authorized use)\r\n"
    "     template: login|terms|update  ssid: open AP name (spaces ok)\r\n"
    "  portal stop | status | ls | creds | clear\r\n"
    "  portal html begin <name> | data <hex> | end | ls | rm <name>  custom HTML\r\n"
    "  led <auto|off|solid|blink|breathe|strobe|rainbow> [r g b]\r\n"
    "  led brightness <0-255>          LED master brightness\r\n"
    "  ir univ <tv|ac|projector|led|fan|audio> <button>  universal remote sweep\r\n"
    "  ir univ stop                    stop universal sweep\r\n"
    "  ir rx <start|stop|clear>         IR receiver (capture + decode)\r\n"
    "  ir spectrum <on|off>            live IR spectrum envelope\r\n"
    "  ir save <name> [capture_id]     save a captured signal to LittleFS\r\n"
    "  ir ls | info <name> | rm <name> saved signal library\r\n"
    "  ir emulate <name> [count] | ir emulate stop   replay a saved signal\r\n"
    "  ir status                       IR module status\r\n"
    "  fs ls | info | rm <path>        browse/delete LittleFS files\r\n"
    "  stop                            stop attack (WiFi or BLE spam)\r\n"
    "  status                          show status\r\n"
    "  wl add <mac> [bssid|sta]        whitelist add (never deauth)\r\n"
    "  wl rm  <mac>                    whitelist remove\r\n"
    "  wl ls                           whitelist list\r\n"
    "  wl clear                        whitelist clear\r\n"
    "  bl add <mac> [bssid|sta]        blacklist add (always deauth)\r\n"
    "  bl rm  <mac>                    blacklist remove\r\n"
    "  bl ls                           blacklist list\r\n"
    "  bl clear                        blacklist clear\r\n"
    "  reset                           soft-reset the chip (esp_restart)\r\n"
    "  help                            this help\r\n";

#define MAX_TOK 32

static int tokenize(char *line, char *tok[MAX_TOK])
{
    int n = 0;
    char *p = line;
    while (*p && n < MAX_TOK) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

static acl_kind_t parse_kind(const char *s)
{
    if (!s) return ACL_KIND_AUTO;
    if (strcasecmp(s, "bssid") == 0) return ACL_KIND_BSSID;
    if (strcasecmp(s, "sta")   == 0) return ACL_KIND_STA;
    return ACL_KIND_AUTO;
}

static void cmd_acl(int argc, char *argv[], bool whitelist)
{
    if (argc < 2) { io_log("usage: %s <add|rm|ls|clear> ...\r\n", argv[0]); return; }
    const char *sub = argv[1];

    if (strcasecmp(sub, "ls") == 0) {
        if (whitelist) acl_wl_list(); else acl_bl_list();
        return;
    }
    if (strcasecmp(sub, "clear") == 0) {
        int rc = whitelist ? acl_wl_clear() : acl_bl_clear();
        io_log(rc == 0 ? "%s: cleared\r\n"
                       : "%s: clear failed (storage)\r\n", argv[0]);
        return;
    }
    if (strcasecmp(sub, "add") == 0) {
        if (argc < 3) { io_log("usage: %s add <mac> [bssid|sta]\r\n", argv[0]); return; }
        uint8_t mac[6];
        if (acl_parse_mac(argv[2], mac) != 0) { io_log("%s: bad mac\r\n", argv[0]); return; }
        acl_kind_t k = (argc >= 4) ? parse_kind(argv[3]) : ACL_KIND_AUTO;
        int rc = whitelist ? acl_wl_add(mac, k) : acl_bl_add(mac, k);
        if      (rc == 0)  io_log("%s: added\r\n", argv[0]);
        else if (rc == -1) io_log("%s: list full\r\n", argv[0]);
        else if (rc == -2) io_log("%s: already present\r\n", argv[0]);
        else if (rc == -3) io_log("%s: add failed (storage)\r\n", argv[0]);
        return;
    }
    if (strcasecmp(sub, "rm") == 0) {
        if (argc < 3) { io_log("usage: %s rm <mac>\r\n", argv[0]); return; }
        uint8_t mac[6];
        if (acl_parse_mac(argv[2], mac) != 0) { io_log("%s: bad mac\r\n", argv[0]); return; }
        int rc = whitelist ? acl_wl_remove(mac) : acl_bl_remove(mac);
        if      (rc == 0)  io_log("%s: removed\r\n", argv[0]);
        else if (rc == -1) io_log("%s: not found\r\n", argv[0]);
        else               io_log("%s: rm failed (storage)\r\n", argv[0]);
        return;
    }
    io_log("%s: unknown subcommand `%s`\r\n", argv[0], sub);
}

/* Emit one LittleFS file entry for the Home file manager (`fs ls`). Paths are
 * quoted so the app can parse names containing any non-quote character. */
static void fs_ls_entry(const char *rel_path, size_t size, void *ctx)
{
    (void)ctx;
    io_log("fs: ENT path=\"%s\" size=%u\r\n", rel_path, (unsigned)size);
}

static void run_line(char *line)
{
    char *argv[MAX_TOK];
    int argc = tokenize(line, argv);
    if (argc == 0) return;

    const char *cmd = argv[0];

    if (wifi_ctrl_scan_is_running() &&
        !(strcasecmp(cmd, "scan") == 0 && argc >= 2 &&
          (strcasecmp(argv[1], "stop") == 0 ||
           strcasecmp(argv[1], "clear") == 0))) {
        io_log("scan: running; use `scan stop` before another command\r\n");
        return;
    }

    if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        io_log("%s", HELP);
        return;
    }
    if (strcasecmp(cmd, "scan") == 0 && argc >= 2 &&
        strcasecmp(argv[1], "stop") == 0) {
        esp_err_t err = wifi_ctrl_scan_stop();
        if (err == ESP_ERR_INVALID_STATE) {
            io_log("scan: not running\r\n");
        } else if (err != ESP_OK) {
            io_log("scan: stop failed (%d)\r\n", (int)err);
        }
        return;
    }
    if (strcasecmp(cmd, "scan") == 0 && argc >= 2 &&
        strcasecmp(argv[1], "clear") == 0) {
        if (wifi_ctrl_scan_is_running()) {
            esp_err_t err = wifi_ctrl_scan_stop();
            if (err != ESP_OK) {
                io_log("scan: clear failed because scan did not stop (%d)\r\n",
                       (int)err);
                return;
            }
        }
        wifi_ctrl_scan_clear();
        return;
    }
    if (strcasecmp(cmd, "scan") == 0) {
        /* A scan retunes the radio across bands; refuse while a WiFi attack
         * owns the radio so we don't corrupt the attack task's channel hop. */
        if (attack_is_running()) {
            io_log("scan: stop the attack first\r\n");
            return;
        }
        if (monitor_is_running()) {
            io_log("scan: stopping monitor first\r\n");
            monitor_stop();
        }
        if (wifi_live_scan_is_running()) {
            io_log("scan: stopping WiFi live scan first\r\n");
            wifi_live_scan_stop();
        }
        if (spectrum_is_running()) {
            io_log("scan: stopping spectrum first\r\n");
            spectrum_stop();
        }
        if (wifi_spam_is_running()) {
            io_log("scan: stopping WiFi spam first\r\n");
            wifi_spam_stop();
        }
        if (evil_portal_is_running()) {
            io_log("scan: stopping evil portal first\r\n");
            evil_portal_stop();
        }
        if (wifi_ctrl_scan_is_running()) {
            io_log("scan: already running\r\n");
            return;
        }
        esp_err_t scan_err = wifi_ctrl_scan_start();
        if (scan_err != ESP_OK) {
            io_log("scan: failed to start (%d)\r\n", (int)scan_err);
        }
        return;
    }
    if (strcasecmp(cmd, "ls") == 0) {
        targets_list_aps();
        return;
    }
    if (strcasecmp(cmd, "stas") == 0) {
        targets_list_stas();
        return;
    }
    if (strcasecmp(cmd, "sniff") == 0) {
        if (argc < 3) { io_log("usage: sniff <ap_idx> <sec>\r\n"); return; }
        int idx = atoi(argv[1]);
        int sec = atoi(argv[2]);
        target_ap_t ap;
        if (targets_get_ap((uint16_t)idx, &ap) != 0) {
            io_log("sniff: bad ap_idx\r\n");
            return;
        }
        sniffer_run(ap.channel, ap.bssid, (uint32_t)sec);
        return;
    }
    if (strcasecmp(cmd, "t24") == 0) {
        if (argc < 2) { io_log("usage: t24 <ap_idx>\r\n"); return; }
        int idx = atoi(argv[1]);
        int rc = targets_select_24((uint16_t)idx);
        if      (rc == 0)  io_log("t24: ok\r\n");
        else if (rc == -1) io_log("t24: that AP is 5 GHz, not 2.4\r\n");
        else               io_log("t24: bad index\r\n");
        return;
    }
    if (strcasecmp(cmd, "t5") == 0) {
        if (argc < 2) { io_log("usage: t5 <ap_idx>\r\n"); return; }
        int idx = atoi(argv[1]);
        int rc = targets_select_5((uint16_t)idx);
        if      (rc == 0)  io_log("t5: ok\r\n");
        else if (rc == -1) io_log("t5: that AP is 2.4 GHz, not 5\r\n");
        else               io_log("t5: bad index\r\n");
        return;
    }
    if (strcasecmp(cmd, "sta") == 0) {
        if (argc < 2) { io_log("usage: sta <sta_idx>\r\n"); return; }
        int idx = atoi(argv[1]);
        int rc = targets_select_sta((uint16_t)idx);
        io_log("sta: %s\r\n", (rc == 0) ? "ok" : "bad index");
        return;
    }
    if (strcasecmp(cmd, "clear") == 0) {
        targets_clear_selection();
        targets_sel_clear();
        io_log("clear: selection reset\r\n");
        return;
    }
    if (strcasecmp(cmd, "sel") == 0) {
        if (argc < 2 || strcasecmp(argv[1], "ls") == 0) {
            targets_list_sel();
            return;
        }
        if (strcasecmp(argv[1], "clear") == 0) {
            targets_sel_clear();
            io_log("sel: cleared\r\n");
            return;
        }
        int idx = atoi(argv[1]);
        if (idx < 0 || idx >= targets_ap_count()) {
            io_log("sel: bad index\r\n");
            return;
        }
        targets_sel_toggle((uint16_t)idx);
        io_log("sel: %d %s\r\n", idx,
               targets_sel_contains((uint16_t)idx) ? "on" : "off");
        return;
    }
    if (strcasecmp(cmd, "mode") == 0) {
        if (argc < 2) {
            io_log("mode: %s\r\n", targets_mode_name(targets_get_mode()));
            return;
        }
        attack_mode_t m;
        if (targets_mode_from_name(argv[1], &m) != 0) {
            io_log("mode: unknown\r\n");
            return;
        }
        targets_set_mode(m);
        io_log("mode: %s\r\n", targets_mode_name(m));
        return;
    }
    if (strcasecmp(cmd, "start") == 0) {
        uint32_t d = (argc >= 2) ? (uint32_t)atoi(argv[1]) : 0;
        attack_start(d);
        return;
    }
    if (strcasecmp(cmd, "nuke") == 0) {
        uint32_t d = (argc >= 2) ? (uint32_t)atoi(argv[1]) : 30;
        attack_nuke_start(d);
        return;
    }
    if (strcasecmp(cmd, "deauthall") == 0 || strcasecmp(cmd, "da") == 0) {
        if (argc >= 2 && strcasecmp(argv[1], "rescan") == 0) {
            attack_deauth_all_rescan();
            io_log("deauthall: rescan queued\r\n");
            return;
        }
        /* Flexible, order-agnostic: --skip (default) | --unicast, plus [sec]. */
        dauth_all_mode_t mode = DAUTH_ALL_MODE_SKIP;
        uint32_t d = 30;
        for (int k = 1; k < argc; k++) {
            if (strcasecmp(argv[k], "--unicast") == 0) {
                mode = DAUTH_ALL_MODE_UNICAST;
            } else if (strcasecmp(argv[k], "--skip") == 0) {
                mode = DAUTH_ALL_MODE_SKIP;
            } else {
                char *endp = NULL;
                long v = strtol(argv[k], &endp, 10);
                if (endp != argv[k] && *endp == '\0' && v >= 0) {
                    d = (uint32_t)v;
                }
            }
        }
        attack_deauth_all_start_mode(d, mode);
        return;
    }
    if (strcasecmp(cmd, "blespam") == 0) {
        if (argc >= 2 && strcasecmp(argv[1], "stop") == 0) {
            ble_spam_stop();
            io_log("blespam: stopped\r\n");
            return;
        }
        if (argc >= 3 && strcasecmp(argv[1], "delay") == 0) {
            ble_spam_set_delay((uint32_t)atoi(argv[2]));
            io_log("blespam: delay=%ums\r\n",
                   (unsigned)ble_spam_current_delay());
            return;
        }
        ble_spam_type_t t = BLE_SPAM_APPLE_JUICE;
        if (argc >= 2) {
            if      (strcasecmp(argv[1], "apple")   == 0) t = BLE_SPAM_APPLE_JUICE;
            else if (strcasecmp(argv[1], "sour")    == 0) t = BLE_SPAM_SOUR_APPLE;
            else if (strcasecmp(argv[1], "samsung") == 0) t = BLE_SPAM_SAMSUNG;
            else if (strcasecmp(argv[1], "google")  == 0) t = BLE_SPAM_GOOGLE_FASTPAIR;
            else if (strcasecmp(argv[1], "windows") == 0) t = BLE_SPAM_WINDOWS_SWIFTPAIR;
            else if (strcasecmp(argv[1], "xiaomi")  == 0) t = BLE_SPAM_XIAOMI_MIBEACON;
            else if (strcasecmp(argv[1], "android") == 0) t = BLE_SPAM_ANDROID;
            else if (strcasecmp(argv[1], "all")     == 0) t = BLE_SPAM_ALL;
        }
        uint32_t delay = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
        if (delay == 0) delay = ble_spam_default_delay(t);
        uint32_t dur = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 300;
        if (ble_wardrive_is_running()) ble_wardrive_stop();
        ble_spam_start(t, delay, dur);
        return;
    }
    if (strcasecmp(cmd, "blescan") == 0) {
        if (argc < 2) {
            io_log("usage: blescan start [active] [sec] | stop | ls | clear | status\r\n");
            return;
        }
        if (strcasecmp(argv[1], "start") == 0) {
            bool active = false;
            uint32_t dur = 30;
            if (argc >= 3 && strcasecmp(argv[2], "active") == 0) {
                active = true;
                if (argc >= 4) dur = (uint32_t)atoi(argv[3]);
            } else if (argc >= 3) {
                dur = (uint32_t)atoi(argv[2]);
            }
            if (dur == 0)   dur = 30;
            if (dur > 600)  dur = 600;
            if (ble_wardrive_is_running()) ble_wardrive_stop();
            ble_scanner_start(dur, active);
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0) {
            ble_scanner_stop();
            return;
        }
        if (strcasecmp(argv[1], "ls") == 0) {
            ble_scanner_dump();
            return;
        }
        if (strcasecmp(argv[1], "clear") == 0) {
            ble_scanner_clear();
            return;
        }
        if (strcasecmp(argv[1], "status") == 0) {
            if (ble_scanner_is_running()) {
                io_log("blescan: running elapsed=%us found=%" PRIu32 "\r\n",
                       (unsigned)ble_scanner_elapsed_sec(),
                       ble_scanner_count());
            } else {
                io_log("blescan: idle count=%" PRIu32 "\r\n",
                       ble_scanner_count());
            }
            return;
        }
        io_log("blescan: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "blefind") == 0) {
        if (argc >= 3 && strcasecmp(argv[1], "save") == 0) {
            esp_err_t err = ble_wardrive_save_findmy(argv[2]);
            if (err == ESP_OK) {
                io_log("blefind: SAVED path=\"findmy_observations.csv\"\r\n");
            } else {
                io_log("blefind: SAVE_ERROR code=%d\r\n", (int)err);
            }
            return;
        }
        io_log("usage: blefind save <address>\r\n");
        return;
    }
    if (strcasecmp(cmd, "blewar") == 0) {
        if (argc < 2) {
            io_log("usage: blewar start [active] [sec] | stop | status\r\n");
            return;
        }
        if (strcasecmp(argv[1], "start") == 0) {
            bool active = false;
            uint32_t dur = 0;   /* 0 = forever */
            if (argc >= 3 && strcasecmp(argv[2], "active") == 0) {
                active = true;
                if (argc >= 4) dur = (uint32_t)atoi(argv[3]);
            } else if (argc >= 3) {
                dur = (uint32_t)atoi(argv[2]);
            }
            if (dur > 3600) dur = 3600;
            ble_wardrive_start(active, dur);
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0) {
            ble_wardrive_stop();
            return;
        }
        if (strcasecmp(argv[1], "status") == 0) {
            if (ble_wardrive_is_running()) {
                io_log("blewar: running cycles=%" PRIu32 " unique=%" PRIu32 "\r\n",
                       ble_wardrive_cycle_count(),
                       ble_wardrive_total_unique());
            } else {
                io_log("blewar: idle\r\n");
            }
            return;
        }
        io_log("blewar: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "monitor") == 0) {
        if (argc < 2) {
            io_log("usage: monitor <ap_idx> | monitor stop\r\n");
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0) {
            monitor_stop();
            return;
        }
        monitor_start((uint16_t)atoi(argv[1]));
        return;
    }
    if (strcasecmp(cmd, "wlscan") == 0) {
        if (argc < 2) {
            io_log("usage: wlscan start <fast|balanced|deep> [itv_ms] [sec] | stop | status\r\n");
            return;
        }
        if (strcasecmp(argv[1], "start") == 0) {
            wlscan_mode_t mode = WLSCAN_MODE_FAST;
            if (argc >= 3) {
                if      (strcasecmp(argv[2], "fast") == 0)     mode = WLSCAN_MODE_FAST;
                else if (strcasecmp(argv[2], "balanced") == 0) mode = WLSCAN_MODE_BALANCED;
                else if (strcasecmp(argv[2], "deep") == 0)     mode = WLSCAN_MODE_DEEP;
                else { io_log("wlscan: unknown mode `%s`\r\n", argv[2]); return; }
            }
            uint32_t interval = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 4000;
            uint32_t dur      = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 0;
            /* Yield conflicting features first. */
            if (attack_is_running())      attack_stop();
            if (monitor_is_running())     monitor_stop();
            if (spectrum_is_running())    spectrum_stop();
            if (wifi_spam_is_running())   wifi_spam_stop();
            if (evil_portal_is_running()) evil_portal_stop();
            if (ble_spam_is_running())    ble_spam_stop();
            if (ble_scanner_is_running()) ble_scanner_stop();
            if (ble_spectrum_is_running())ble_spectrum_stop();
            if (ble_wardrive_is_running())ble_wardrive_stop();
            wifi_live_scan_start(mode, interval, dur);
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0) {
            wifi_live_scan_stop();
            return;
        }
        if (strcasecmp(argv[1], "status") == 0) {
            if (wifi_live_scan_is_running()) {
                io_log("wlscan: running cycles=%" PRIu32 " unique=%" PRIu32 "\r\n",
                       wifi_live_scan_cycle_count(),
                       wifi_live_scan_total_unique());
            } else {
                io_log("wlscan: idle\r\n");
            }
            return;
        }
        io_log("wlscan: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "spectrum") == 0) {
        if (argc >= 2 && strcasecmp(argv[1], "stop") == 0) {
            spectrum_stop();
            return;
        }
        if (argc >= 2 && strcasecmp(argv[1], "status") == 0) {
            io_log("spectrum: %s\r\n",
                   spectrum_is_running() ? "running" : "idle");
            return;
        }
        /* start [5] */
        bool incl5 = false;
        if (argc >= 3 && strcasecmp(argv[2], "5") == 0) incl5 = true;
        else if (argc >= 2 && strcasecmp(argv[1], "5") == 0) incl5 = true;
        spectrum_start(incl5);
        return;
    }
    if (strcasecmp(cmd, "blespectrum") == 0) {
        if (argc >= 2 && strcasecmp(argv[1], "stop") == 0) {
            ble_spectrum_stop();
            return;
        }
        if (argc >= 2 && strcasecmp(argv[1], "status") == 0) {
            io_log("blespectrum: %s\r\n",
                   ble_spectrum_is_running() ? "running" : "idle");
            return;
        }
        if (ble_wardrive_is_running()) ble_wardrive_stop();
        ble_spectrum_start();
        return;
    }
    if (strcasecmp(cmd, "nrf") == 0) {
        if (!nrf24_module_ready()) {
            io_log("nrf: module not available\r\n");
            return;
        }
        if (argc < 2) {
            io_log("usage: nrf <scan|waterfall|wfjam|jam|sniff|replay|mj|status|stopall>\r\n");
            return;
        }

        if (strcasecmp(argv[1], "stopall") == 0) {
            nrf24_scanner_waterfall_stop();
            nrf24_jammer_stop();
            nrf24_sniffer_stop();
            nrf24_mousejack_scan_stop();
            io_log("nrf: stopall issued\r\n");
            return;
        }

        if (strcasecmp(argv[1], "scan") == 0) {
            if (nrf24_radio_busy()) { io_log("nrf: radio busy\r\n"); return; }
            uint8_t peak_ch = 0;
            int strength = nrf24_scanner_peak(&peak_ch);
            if (strength < 0) io_log("nrf: scan failed\r\n");
            else io_log("nrf: peak ch=%u rpd=%d/32\r\n",
                        (unsigned)peak_ch, strength);
            return;
        }

        if (strcasecmp(argv[1], "waterfall") == 0) {
            if (argc >= 3 && strcasecmp(argv[2], "stop") == 0)
                nrf24_scanner_waterfall_stop();
            else
                nrf24_scanner_waterfall_start();
            return;
        }

        if (strcasecmp(argv[1], "wfjam") == 0 ||
            strcasecmp(argv[1], "waterfalljam") == 0) {
            uint32_t dur = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
            nrf24_jammer_start_waterfall(dur);
            return;
        }

        if (strcasecmp(argv[1], "ls") == 0) { nrf24_scanner_dump(); return; }

        if (strcasecmp(argv[1], "jam") == 0) {
            if (argc < 3) { io_log("usage: nrf jam <auto|manual|stop>\r\n"); return; }
            if (strcasecmp(argv[2], "stop") == 0) { nrf24_jammer_stop(); return; }

            if (strcasecmp(argv[2], "auto") == 0) {
                if (argc < 4) { io_log("usage: nrf jam auto <target> [sec]\r\n"); return; }
                jam_target_t t = JAM_TARGET_FULL;
                if      (strcasecmp(argv[3], "wifi")       == 0) t = JAM_TARGET_WIFI;
                else if (strcasecmp(argv[3], "ble_adv")    == 0) t = JAM_TARGET_BLE_ADV;
                else if (strcasecmp(argv[3], "ble_data")   == 0) t = JAM_TARGET_BLE_DATA;
                else if (strcasecmp(argv[3], "bt_classic") == 0) t = JAM_TARGET_BT_CLASSIC;
                else if (strcasecmp(argv[3], "zigbee")     == 0) t = JAM_TARGET_ZIGBEE;
                else if (strcasecmp(argv[3], "drone")      == 0) t = JAM_TARGET_DRONE;
                else if (strcasecmp(argv[3], "usb_dongle") == 0) t = JAM_TARGET_USB_DONGLE;
                uint32_t dur = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 0;
                nrf24_jammer_start_auto(t, dur);
                return;
            }

            if (strcasecmp(argv[2], "manual") == 0) {
                if (argc < 9) {
                    io_log("usage: nrf jam manual <target> <strat> <rate> <pa> <dwell> <burst> [sec]\r\n");
                    return;
                }
                jam_config_t cfg;
                nrf24_jammer_default_config(JAM_TARGET_FULL, &cfg);

                if      (strcasecmp(argv[3], "wifi")       == 0) cfg.target = JAM_TARGET_WIFI;
                else if (strcasecmp(argv[3], "ble_adv")    == 0) cfg.target = JAM_TARGET_BLE_ADV;
                else if (strcasecmp(argv[3], "ble_data")   == 0) cfg.target = JAM_TARGET_BLE_DATA;
                else if (strcasecmp(argv[3], "bt_classic") == 0) cfg.target = JAM_TARGET_BT_CLASSIC;
                else if (strcasecmp(argv[3], "zigbee")     == 0) cfg.target = JAM_TARGET_ZIGBEE;
                else if (strcasecmp(argv[3], "drone")      == 0) cfg.target = JAM_TARGET_DRONE;
                else if (strcasecmp(argv[3], "usb_dongle") == 0) cfg.target = JAM_TARGET_USB_DONGLE;

                if      (strcasecmp(argv[4], "flood")    == 0) cfg.strategy = JAM_STRAT_FLOOD;
                else if (strcasecmp(argv[4], "reactive") == 0) cfg.strategy = JAM_STRAT_REACTIVE;
                else                                           cfg.strategy = JAM_STRAT_CW;

                if      (strcasecmp(argv[5], "250k") == 0) cfg.rate = NRF24_RATE_250K;
                else if (strcasecmp(argv[5], "2m")   == 0) cfg.rate = NRF24_RATE_2M;
                else                                       cfg.rate = NRF24_RATE_1M;

                if      (strcasecmp(argv[6], "min")  == 0) cfg.pa = NRF24_PA_MIN;
                else if (strcasecmp(argv[6], "low")  == 0) cfg.pa = NRF24_PA_LOW;
                else if (strcasecmp(argv[6], "high") == 0) cfg.pa = NRF24_PA_HIGH;
                else                                       cfg.pa = NRF24_PA_MAX;

                cfg.dwell_us = (uint32_t)atoi(argv[7]);
                if (cfg.dwell_us < 50)    cfg.dwell_us = 50;
                if (cfg.dwell_us > 50000) cfg.dwell_us = 50000;

                cfg.burst = (uint8_t)atoi(argv[8]);
                if (cfg.burst == 0) cfg.burst = 1;
                if (cfg.burst > 20) cfg.burst = 20;

                uint32_t dur = (argc >= 10) ? (uint32_t)atoi(argv[9]) : 0;
                nrf24_jammer_start_manual(&cfg, dur);
                return;
            }
            io_log("nrf jam: unknown\r\n");
            return;
        }

        if (strcasecmp(argv[1], "sniff") == 0) {
            if (argc < 3) { io_log("usage: nrf sniff <start|stop|ls|clear>\r\n"); return; }
            if (strcasecmp(argv[2], "start") == 0) {
                uint8_t ch = (argc >= 4) ? (uint8_t)atoi(argv[3]) : 76;
                uint32_t sec = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 30;
                nrf24_sniffer_start(ch, sec);
                return;
            }
            if (strcasecmp(argv[2], "stop") == 0)  { nrf24_sniffer_stop();  return; }
            if (strcasecmp(argv[2], "ls") == 0)    { nrf24_sniffer_dump();  return; }
            if (strcasecmp(argv[2], "clear") == 0) { nrf24_sniffer_clear(); return; }
            io_log("nrf sniff: unknown\r\n");
            return;
        }

        if (strcasecmp(argv[1], "replay") == 0) {
            if (argc < 3) { io_log("usage: nrf replay <idx> [count] [delay_us]\r\n"); return; }
            uint32_t idx = (uint32_t)atoi(argv[2]);
            uint32_t cnt = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 1;
            uint32_t del = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 1000;
            nrf24_sniffer_replay(idx, cnt, del);
            return;
        }

        if (strcasecmp(argv[1], "mj") == 0) {
            if (argc < 3) { io_log("usage: nrf mj <scan|stop|ls|clear|inject>\r\n"); return; }
            if (strcasecmp(argv[2], "scan") == 0) {
                uint32_t sec = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 30;
                nrf24_mousejack_scan_start(sec);
                return;
            }
            if (strcasecmp(argv[2], "stop") == 0)  { nrf24_mousejack_scan_stop(); return; }
            if (strcasecmp(argv[2], "ls") == 0)    { nrf24_mousejack_dump();      return; }
            if (strcasecmp(argv[2], "clear") == 0) { nrf24_mousejack_clear();     return; }
            if (strcasecmp(argv[2], "inject") == 0) {
                if (argc < 5) { io_log("usage: nrf mj inject <idx> <text...>\r\n"); return; }
                uint32_t idx = (uint32_t)atoi(argv[3]);

                /* FIX L3: gabung argv[4..argc-1] dengan spasi */
                char text[128] = {0};
                size_t off = 0;
                for (int k = 4; k < argc; k++) {
                    size_t l = strlen(argv[k]);
                    if (off + l + 2 >= sizeof(text)) break;
                    if (off > 0) text[off++] = ' ';
                    memcpy(text + off, argv[k], l);
                    off += l;
                }
                text[off] = '\0';
                if (off == 0) { io_log("nrf mj: empty text\r\n"); return; }

                nrf24_mousejack_inject(idx, text);
                return;
            }
            io_log("nrf mj: unknown\r\n");
            return;
        }

        if (strcasecmp(argv[1], "status") == 0) {
            nrf24_dump_status();
            io_log("nrf: wf=%d jam=%d sniff=%d mj=%d radio_busy=%d\r\n",
                   (int)nrf24_scanner_waterfall_is_running(),
                   (int)nrf24_jammer_is_running(),
                   (int)nrf24_sniffer_is_running(),
                   (int)nrf24_mousejack_scan_is_running(),
                   (int)nrf24_radio_busy());
            return;
        }

        io_log("nrf: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "wspam") == 0) {
        if (argc < 2) {
            io_log("usage: wspam start <mode> [rate] [band] [sec] | stop | status | add <ssid> | clear | ls\r\n");
            return;
        }
        if (strcasecmp(argv[1], "start") == 0) {
            if (argc < 3) {
                io_log("usage: wspam start <funny|rickroll|random|all|custom> [aggressive|normal|stealth] [2g|dual] [sec]\r\n");
                return;
            }
            wspam_mode_t mode;
            if      (strcasecmp(argv[2], "funny")    == 0) mode = WSPAM_MODE_FUNNY;
            else if (strcasecmp(argv[2], "rickroll") == 0) mode = WSPAM_MODE_RICKROLL;
            else if (strcasecmp(argv[2], "random")   == 0) mode = WSPAM_MODE_RANDOM;
            else if (strcasecmp(argv[2], "all")      == 0) mode = WSPAM_MODE_ALL;
            else if (strcasecmp(argv[2], "custom")   == 0) mode = WSPAM_MODE_CUSTOM;
            else { io_log("wspam: unknown mode `%s`\r\n", argv[2]); return; }

            wspam_rate_t rate = WSPAM_RATE_AGGRESSIVE;
            bool band_5g = true;
            uint32_t dur = 0;
            for (int k = 3; k < argc; k++) {
                if      (strcasecmp(argv[k], "aggressive") == 0) rate = WSPAM_RATE_AGGRESSIVE;
                else if (strcasecmp(argv[k], "normal") == 0)     rate = WSPAM_RATE_NORMAL;
                else if (strcasecmp(argv[k], "stealth") == 0)    rate = WSPAM_RATE_STEALTH;
                else if (strcasecmp(argv[k], "2g") == 0)         band_5g = false;
                else if (strcasecmp(argv[k], "dual") == 0)       band_5g = true;
                else {
                    char *endp = NULL;
                    long v = strtol(argv[k], &endp, 10);
                    if (endp != argv[k] && *endp == '\0' && v >= 0) {
                        dur = (uint32_t)v;
                    } else {
                        io_log("wspam: unknown arg `%s`\r\n", argv[k]);
                        return;
                    }
                }
            }

            /* Yield conflicting features (WiFi + BLE radio users). */
            if (attack_is_running())       attack_stop();
            if (monitor_is_running())      monitor_stop();
            if (spectrum_is_running())     spectrum_stop();
            if (wifi_live_scan_is_running())wifi_live_scan_stop();
            if (evil_portal_is_running())  evil_portal_stop();
            if (ble_spam_is_running())     ble_spam_stop();
            if (ble_scanner_is_running())  ble_scanner_stop();
            if (ble_spectrum_is_running()) ble_spectrum_stop();
            if (ble_wardrive_is_running()) ble_wardrive_stop();

            wifi_spam_start(mode, rate, band_5g, dur);
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0) {
            wifi_spam_stop();
            return;
        }
        if (strcasecmp(argv[1], "status") == 0) {
            if (wifi_spam_is_running()) {
                io_log("wspam: running mode=%s rate=%s band=%s elapsed=%us "
                       "total=%" PRIu32 " ch=%u\r\n",
                       wifi_spam_mode_name(wifi_spam_current_mode()),
                       wifi_spam_rate_name(wifi_spam_current_rate()),
                       wifi_spam_current_dual_band() ? "dual" : "2.4G",
                       (unsigned)wifi_spam_elapsed_sec(),
                       wifi_spam_total_sent(),
                       (unsigned)wifi_spam_current_channel());
            } else {
                io_log("wspam: idle custom=%u\r\n",
                       (unsigned)wifi_spam_custom_count());
            }
            return;
        }
        if (strcasecmp(argv[1], "add") == 0) {
            if (argc < 3) { io_log("usage: wspam add <ssid>\r\n"); return; }
            /* Rejoin tokens so multi-word SSIDs survive tokenization. */
            char ssid[64];
            size_t pos = 0;
            for (int k = 2; k < argc && pos < sizeof(ssid) - 1; k++) {
                if (k > 2 && pos < sizeof(ssid) - 1) ssid[pos++] = ' ';
                for (size_t m = 0; argv[k][m] && pos < sizeof(ssid) - 1; m++) {
                    ssid[pos++] = argv[k][m];
                }
            }
            ssid[pos] = '\0';
            esp_err_t r = wifi_spam_custom_add(ssid);
            if (r == ESP_OK) {
                io_log("wspam: added \"%s\" (count=%u)\r\n",
                       ssid, (unsigned)wifi_spam_custom_count());
            } else if (r == ESP_ERR_INVALID_STATE) {
                io_log("wspam: \"%s\" already exists\r\n", ssid);
            } else {
                io_log("wspam: add failed (%d)\r\n", r);
            }
            return;
        }
        if (strcasecmp(argv[1], "clear") == 0) {
            esp_err_t r = wifi_spam_custom_clear();
            io_log("wspam: clear %s\r\n", r == ESP_OK ? "ok" : "failed");
            return;
        }
        if (strcasecmp(argv[1], "ls") == 0) {
            uint32_t n = wifi_spam_custom_count();
            io_log("wspam: LS count=%u\r\n", (unsigned)n);
            for (uint32_t i = 0; i < n; i++) {
                char buf[64];
                if (wifi_spam_custom_get(i, buf, sizeof(buf)) == ESP_OK) {
                    io_log("wspam: LS idx=%u ssid=\"%s\"\r\n", (unsigned)i, buf);
                }
            }
            io_log("wspam: LS end\r\n");
            return;
        }
        io_log("wspam: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "portal") == 0) {
        if (argc < 2) {
            io_log("usage: portal <start|stop|status|ls|creds|clear>\r\n");
            return;
        }
        if (strcasecmp(argv[1], "start") == 0) {
            if (argc < 3) {
                io_log("usage: portal start <template> [ssid]\r\n");
                return;
            }
            const char *tmpl = argv[2];
            /* Rejoin argv[3..] so multi-word SSIDs survive tokenization. */
            char ssid[EVIL_PORTAL_SSID_MAX + 1];
            size_t pos = 0;
            for (int k = 3; k < argc && pos < sizeof(ssid) - 1; k++) {
                if (k > 3 && pos < sizeof(ssid) - 1) ssid[pos++] = ' ';
                for (size_t m = 0; argv[k][m] && pos < sizeof(ssid) - 1; m++) {
                    ssid[pos++] = argv[k][m];
                }
            }
            ssid[pos] = '\0';
            evil_portal_start(tmpl, pos ? ssid : NULL, 1);
            return;
        }
        if (strcasecmp(argv[1], "stop") == 0)   { evil_portal_stop(); return; }
        if (strcasecmp(argv[1], "status") == 0) { evil_portal_status(); return; }
        if (strcasecmp(argv[1], "ls") == 0)     { evil_portal_list_templates(); return; }
        if (strcasecmp(argv[1], "creds") == 0)  { evil_portal_dump_creds(); return; }
        if (strcasecmp(argv[1], "clear") == 0)  { evil_portal_clear_creds(); return; }
        if (strcasecmp(argv[1], "html") == 0) {
            if (argc < 3) {
                io_log("usage: portal html <begin <name>|data <hex>|end|ls|rm <name>>\r\n");
                return;
            }
            if (strcasecmp(argv[2], "begin") == 0) {
                if (argc < 4) { io_log("usage: portal html begin <name>\r\n"); return; }
                evil_portal_html_begin(argv[3]);
                return;
            }
            if (strcasecmp(argv[2], "data") == 0) {
                if (argc < 4) { io_log("usage: portal html data <hex>\r\n"); return; }
                evil_portal_html_data(argv[3]);
                return;
            }
            if (strcasecmp(argv[2], "end") == 0) { evil_portal_html_end(); return; }
            if (strcasecmp(argv[2], "ls") == 0)  { evil_portal_html_list(); return; }
            if (strcasecmp(argv[2], "rm") == 0) {
                if (argc < 4) { io_log("usage: portal html rm <name>\r\n"); return; }
                evil_portal_html_remove(argv[3]);
                return;
            }
            io_log("portal: unknown html subcommand `%s`\r\n", argv[2]);
            return;
        }
        io_log("portal: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "led") == 0) {
        if (argc < 2) { io_log("usage: led <auto|off|solid|blink|breathe|strobe|rainbow> [r g b] | led brightness <0-255>\r\n"); return; }
        if (strcasecmp(argv[1], "brightness") == 0) {
            if (argc < 3) { io_log("usage: led brightness <0-255>\r\n"); return; }
            int v = atoi(argv[2]);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            led_set_brightness((uint8_t)v);
            io_log("led: brightness=%d\r\n", v);
            return;
        }
        if (strcasecmp(argv[1], "auto") == 0) {
            led_set_auto();
            io_log("led: auto\r\n");
            return;
        }
        led_effect_t eff;
        if      (strcasecmp(argv[1], "off") == 0)     eff = LED_EFFECT_OFF;
        else if (strcasecmp(argv[1], "solid") == 0)   eff = LED_EFFECT_SOLID;
        else if (strcasecmp(argv[1], "blink") == 0)   eff = LED_EFFECT_BLINK;
        else if (strcasecmp(argv[1], "breathe") == 0) eff = LED_EFFECT_BREATHE;
        else if (strcasecmp(argv[1], "strobe") == 0)  eff = LED_EFFECT_STROBE;
        else if (strcasecmp(argv[1], "rainbow") == 0) eff = LED_EFFECT_RAINBOW;
        else { io_log("led: unknown effect `%s`\r\n", argv[1]); return; }
        uint8_t r = 255, g = 255, b = 255;
        if (argc >= 5) {
            int ri = atoi(argv[2]), gi = atoi(argv[3]), bi = atoi(argv[4]);
            r = (uint8_t)(ri < 0 ? 0 : ri > 255 ? 255 : ri);
            g = (uint8_t)(gi < 0 ? 0 : gi > 255 ? 255 : gi);
            b = (uint8_t)(bi < 0 ? 0 : bi > 255 ? 255 : bi);
        }
        led_set_effect(eff, r, g, b);
        io_log("led: effect=%s rgb=%u,%u,%u\r\n", argv[1], r, g, b);
        return;
    }
    if (strcasecmp(cmd, "ir") == 0) {
        if (!ir_module_ready()) { io_log("ir: module not available\r\n"); return; }
        if (argc < 2) {
            io_log("usage: ir <univ|rx|spectrum|save|ls|info|rm|emulate|status>\r\n");
            return;
        }
        if (strcasecmp(argv[1], "univ") == 0) {
            if (argc >= 3 && strcasecmp(argv[2], "stop") == 0) {
                ir_universal_stop();
                return;
            }
            if (argc < 4) {
                io_log("usage: ir univ <tv|ac|projector|led|fan|audio> <button>\r\n");
                return;
            }
            ir_cat_t cat = ir_cat_from_name(argv[2]);
            if (cat >= IR_CAT_COUNT) { io_log("ir: unknown category `%s`\r\n", argv[2]); return; }
            esp_err_t err = ir_universal_send(cat, argv[3]);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                io_log("ir: universal start failed (%d)\r\n", (int)err);
            }
            return;
        }
        if (strcasecmp(argv[1], "rx") == 0) {
            if (argc >= 3 && strcasecmp(argv[2], "stop") == 0) {
                ir_rx_stop();
            } else if (argc >= 3 && strcasecmp(argv[2], "clear") == 0) {
                if (ir_capture_history_clear() != ESP_OK) {
                    io_log("ir: capture history clear failed\r\n");
                }
            } else {
                ir_rx_start();
            }
            return;
        }
        if (strcasecmp(argv[1], "spectrum") == 0) {
            bool on = !(argc >= 3 && strcasecmp(argv[2], "off") == 0);
            ir_spectrum_set(on);
            return;
        }
        if (strcasecmp(argv[1], "save") == 0) {
            if (argc < 3) {
                io_log("usage: ir save <name> [capture_id]\r\n");
                return;
            }
            /* Single token only, so `ir rm/info/emulate <name>` (which use
             * argv[2]) always match. The app sends space-free names; the
             * firmware also sanitizes on write. */
            uint32_t capture_id = argc >= 4
                ? (uint32_t)strtoul(argv[3], NULL, 10)
                : 0;
            ir_save_capture(argv[2], capture_id);
            return;
        }
        if (strcasecmp(argv[1], "ls") == 0)   { ir_saved_list(); return; }
        if (strcasecmp(argv[1], "info") == 0) {
            if (argc < 3) { io_log("usage: ir info <name>\r\n"); return; }
            ir_saved_info(argv[2]);
            return;
        }
        if (strcasecmp(argv[1], "rm") == 0) {
            if (argc < 3) { io_log("usage: ir rm <name>\r\n"); return; }
            if (ir_saved_delete(argv[2]) != ESP_OK) io_log("ir: rm not found\r\n");
            return;
        }
        if (strcasecmp(argv[1], "emulate") == 0) {
            if (argc >= 3 && strcasecmp(argv[2], "stop") == 0) {
                ir_emulate_stop();
                return;
            }
            if (argc < 3) { io_log("usage: ir emulate <name> [count]\r\n"); return; }
            int count = (argc >= 4) ? atoi(argv[3]) : 0;
            esp_err_t err = ir_emulate_start(argv[2], count);
            if (err != ESP_OK) {
                io_log("ir: emit start failed (%d)\r\n", (int)err);
            }
            return;
        }
        if (strcasecmp(argv[1], "status") == 0) { ir_status(); return; }
        io_log("ir: unknown subcommand `%s`\r\n", argv[1]);
        return;
    }
    if (strcasecmp(cmd, "stop") == 0) {
        /* WiFi-attack stop only. BLE spam, BLE scan and the monitor each
         * have their own stop command (blespam stop / blescan stop /
         * monitor stop) and run independently, so `stop` must not touch
         * them — matches DeviceController.stopAttack()'s contract. */
        attack_stop();
        return;
    }
    if (strcasecmp(cmd, "status") == 0) {
        attack_print_status();
        if (ble_spam_is_running()) {
            io_log("blespam: running type=%s delay=%ums frames=%" PRIu32 "\r\n",
                   ble_spam_type_name(ble_spam_current_type()),
                   (unsigned)ble_spam_current_delay(),
                   ble_spam_frames_sent());
        }
        return;
    }
    if (strcasecmp(cmd, "wl") == 0) { cmd_acl(argc, argv, true);  return; }
    if (strcasecmp(cmd, "bl") == 0) { cmd_acl(argc, argv, false); return; }

    if (strcasecmp(cmd, "dev") == 0) {
        if (argc < 2) { io_log("usage: dev scan [sec] | dev ls\r\n"); return; }
        if (strcasecmp(argv[1], "ls") == 0) {
            targets_list_stas_machine();
        } else if (strcasecmp(argv[1], "scan") == 0) {
            uint32_t sec = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 13;
            targets_clear_stas();
            if (sniffer_sweep(sec) < 0) {
                io_log("dev: scan failed\r\n");
                return;
            }
            targets_list_stas_machine();
        } else {
            io_log("usage: dev scan [sec] | dev ls\r\n");
        }
        return;
    }

    if (strcasecmp(cmd, "fs") == 0) {
        if (!storage_fs_ready()) storage_fs_mount();
        if (!storage_fs_ready()) { io_log("fs: not ready\r\n"); return; }
        const char *sub = (argc >= 2) ? argv[1] : "ls";
        if (strcasecmp(sub, "ls") == 0) {
            size_t total = 0, used = 0;
            storage_fs_info(&total, &used);
            io_log("fs: LS total=%u used=%u\r\n",
                   (unsigned)total, (unsigned)used);
            storage_fs_walk(fs_ls_entry, NULL);
            io_log("fs: LS end\r\n");
            return;
        }
        if (strcasecmp(sub, "info") == 0) {
            size_t total = 0, used = 0;
            storage_fs_info(&total, &used);
            io_log("fs: INFO total=%u used=%u free=%u\r\n",
                   (unsigned)total, (unsigned)used,
                   (unsigned)(total >= used ? total - used : 0));
            return;
        }
        if (strcasecmp(sub, "rm") == 0) {
            if (argc < 3) { io_log("usage: fs rm <path>\r\n"); return; }
            esp_err_t e = storage_fs_delete(argv[2]);
            if (e == ESP_OK) {
                io_log("fs: RM path=\"%s\" ok\r\n", argv[2]);
            } else if (e == ESP_ERR_NOT_FOUND) {
                io_log("fs: RM path=\"%s\" notfound\r\n", argv[2]);
            } else {
                io_log("fs: RM path=\"%s\" bad\r\n", argv[2]);
            }
            return;
        }
        io_log("usage: fs <ls|info|rm <path>>\r\n");
        return;
    }

    if (strcasecmp(cmd, "reset") == 0 || strcasecmp(cmd, "reboot") == 0) {
        io_log("reset: restarting chip in 200ms\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    io_log("unknown: %s (try `help`)\r\n", cmd);
}

static void command_task(void *arg)
{
    (void)arg;
    cli_command_t command;

    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            run_line(command.line);
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool overflow = s_command_queue_overflow;
        s_command_queue_overflow = false;
        xSemaphoreGive(s_mutex);

        if (overflow) {
            io_log("cli: command queue full; input dropped\r\n");
        }
    }
}

void cli_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_command_queue = xQueueCreate(CLI_COMMAND_QUEUE_LENGTH, sizeof(cli_command_t));
    s_len = 0;
    s_command_queue_overflow = false;

    ESP_ERROR_CHECK(s_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_command_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(command_task, "cli_cmd", 4096, NULL, 6, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}

void cli_feed(const char *bytes, size_t len)
{
    if (!bytes || len == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_len] = '\0';
            cli_command_t command;
            memcpy(command.line, s_line, s_len + 1);
            s_len = 0;
            if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
                s_command_queue_overflow = true;
            }
            continue;
        }
        if (c == 0x08 || c == 0x7f) {
            if (s_len > 0) s_len--;
            continue;
        }
        if (s_len < CLI_LINE_MAX - 1) {
            s_line[s_len++] = c;
        } else {
            s_len = 0;
        }
    }
    xSemaphoreGive(s_mutex);
}
