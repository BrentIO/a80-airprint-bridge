// Wi-Fi: joins the owner's home network when one is saved, otherwise (or when it can't) runs the open "A80-Print"
// hotspot. A setup page lists the networks in range and saves a new home network once the bridge has really joined it.
//
// Modes:
//   M_HOME     station only, on the home network (hotspot off)
//   M_LOST     station only, home network dropped: reconnecting for A80_HOME_LOST_GRACE_S, then the hotspot starts
//   M_HOTSPOT  hotspot on; the home network is retried every A80_HOME_RETRY_S, but only while no phone is on the
//              hotspot (a connection attempt scans other channels, which would interrupt a phone's transfer)
//
// A new network is saved to flash only after the bridge has actually joined it, so a wrong password or an
// out-of-range network never replaces a working setting. The password is never logged.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "config.h"
#include "formparse.h"
#include "weblog.h"
#include "wifi.h"

#define GOT_IP BIT0

typedef enum { M_HOME, M_LOST, M_HOTSPOT } wmode_t;

static esp_netif_t *sta_if, *ap_if;
static EventGroupHandle_t wev;
static SemaphoreHandle_t lock;   // guards home_* and pend_* (the HTTP workers and the Wi-Fi task both touch them)
static volatile wmode_t mode = M_HOTSPOT;
static volatile bool sta_ip, joining, ap_on, have_home, forget_req, pend_ready;
static volatile int last_reason;   // reason code of the last station disconnect
static char home_ssid[33], home_pass[65];
static char pend_ssid[33], pend_pass[65];
static int64_t lost_us, last_try_us, drop_ap_us;

// Outcome of the last change made on the setup page, read by the HTTP worker that is waiting to answer the phone.
typedef enum { JR_IDLE, JR_PENDING, JR_OK, JR_FAIL } jstate_t;
static volatile jstate_t join_state = JR_IDLE;
static char join_ip[16];
static const char *join_why = "";

// ---------------------------------------------------------------- saved network (flash)
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof home_ssid;
    if (nvs_get_str(h, "ssid", home_ssid, &n) == ESP_OK && home_ssid[0]) {
        n = sizeof home_pass;
        if (nvs_get_str(h, "pass", home_pass, &n) != ESP_OK) home_pass[0] = 0;
        have_home = true;
    }
    nvs_close(h);
}

static void nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- hotspot and station
static int ap_clients(void)
{
    wifi_sta_list_t l;
    return ap_on && esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

static void hotspot_start(void)
{
    wifi_config_t ap = {.ap = {.ssid = A80_AP_SSID, .ssid_len = strlen(A80_AP_SSID),
                               .channel = A80_AP_CHANNEL, .max_connection = A80_AP_MAX_CONN,
                               .authmode = WIFI_AUTH_OPEN}};
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_set_max_tx_power(A80_AP_TX_POWER_DBM * 4);   // units of 0.25 dBm; applies to the whole radio, so it's restored in hotspot_stop
    ap_on = true;
    weblog("Wi-Fi hotspot \"%s\" is on (no password, reduced range: %d dBm) [address %s]", A80_AP_SSID, A80_AP_TX_POWER_DBM, A80_AP_IP);
}

static void hotspot_stop(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_max_tx_power(80);   // back to the maximum for the link to the home router
    ap_on = false;
}

static void sta_config(const char *ssid, const char *pass)
{
    wifi_config_t c = {0};
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass, sizeof c.sta.password);
    c.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

// Try a network and wait for an address. The station's retries run in the event handler while `joining` is set.
static bool try_join(const char *ssid, const char *pass, int secs)
{
    xEventGroupClearBits(wev, GOT_IP);
    sta_ip = false;
    last_reason = 0;
    joining = true;
    sta_config(ssid, pass);   // before the disconnect: the retry in the event handler must use the new network
    esp_wifi_disconnect();
    esp_wifi_connect();
    bool ok = xEventGroupWaitBits(wev, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(secs * 1000)) & GOT_IP;
    joining = false;
    if (ok) {   // belt and braces: an address on the *old* network must never count as joining the new one
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || strcmp((const char *)ap.ssid, ssid)) ok = false;
    }
    return ok;
}

static const char *why_not(int reason)
{
    switch (reason) {
    case 201: return "network not found";
    case 2: case 15: case 202: case 204: case 205: return "wrong password?";
    default: return "no answer";
    }
}

void wifi_address(char *out, size_t cap)
{
    esp_netif_ip_info_t ip;
    if (sta_ip && sta_if && esp_netif_get_ip_info(sta_if, &ip) == ESP_OK) snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    else snprintf(out, cap, "%s", A80_AP_IP);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = data;
        weblog("Wi-Fi: gave the client address " IPSTR, IP2STR(&e->ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        sta_ip = true;
        xEventGroupSetBits(wev, GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        last_reason = e->reason;
        sta_ip = false;
        xEventGroupClearBits(wev, GOT_IP);
        if (joining) {
            esp_wifi_connect();   // keep trying until try_join() gives up
        } else if (mode == M_HOME && have_home) {
            mode = M_LOST;
            lost_us = esp_timer_get_time();
            weblog("Wi-Fi: lost the home network, reconnecting [%s]", why_not(e->reason));
            esp_wifi_connect();
        } else if (mode == M_LOST) {
            esp_wifi_connect();   // quick retries during the grace period
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        weblog("Wi-Fi: a client joined the hotspot [MAC %02x:%02x:%02x:%02x:%02x:%02x]", e->mac[0], e->mac[1], e->mac[2],
               e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        weblog("Wi-Fi: a client left the hotspot");
    }
}

// ---------------------------------------------------------------- background task
static void apply_requested_change(void)
{
    char ssid[33], pass[65];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(ssid, pend_ssid, sizeof ssid);
    strlcpy(pass, pend_pass, sizeof pass);
    pend_ssid[0] = pend_pass[0] = 0;
    xSemaphoreGive(lock);
    pend_ready = false;
    if (!ssid[0]) {
        join_state = JR_IDLE;
        return;
    }

    weblog("Wi-Fi: trying to join \"%s\" (up to %d s)", ssid, A80_HOME_JOIN_S);
    if (try_join(ssid, pass, A80_HOME_JOIN_S)) {
        nvs_save(ssid, pass);
        xSemaphoreTake(lock, portMAX_DELAY);
        strlcpy(home_ssid, ssid, sizeof home_ssid);
        strlcpy(home_pass, pass, sizeof home_pass);
        xSemaphoreGive(lock);
        have_home = true;
        weblog("Wi-Fi: joined \"%s\" and saved it as the home network", ssid);
        wifi_address(join_ip, sizeof join_ip);
        join_state = JR_OK;   // the phone's page is waiting for this
        if (ap_on) {
            mode = M_HOTSPOT;
            drop_ap_us = esp_timer_get_time() + A80_SETUP_RESULT_S * 1000000LL;   // keep the hotspot until the result is read
        } else {
            mode = M_HOME;
        }
    } else {
        weblog("Wi-Fi: couldn't join \"%s\" (%s); the previous setting is kept", ssid, why_not(last_reason));
        join_why = why_not(last_reason);
        join_state = JR_FAIL;   // the phone's page is waiting for this
        if (have_home) {
            sta_config(home_ssid, home_pass);   // back to the saved network (retried by the usual rules)
            esp_wifi_disconnect();
            if (!ap_on) {
                mode = M_LOST;
                lost_us = esp_timer_get_time();
                esp_wifi_connect();
            }
        } else {
            esp_wifi_disconnect();   // nothing saved: stop trying the network that failed
            if (!ap_on) {
                hotspot_start();   // and never leave the bridge without a network
                mode = M_HOTSPOT;
            }
        }
    }
    memset(pass, 0, sizeof pass);
}

static void wifi_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();

        if (forget_req) {
            forget_req = false;
            nvs_forget();
            xSemaphoreTake(lock, portMAX_DELAY);
            home_ssid[0] = home_pass[0] = 0;
            xSemaphoreGive(lock);
            have_home = false;
            esp_wifi_disconnect();
            if (!ap_on) hotspot_start();
            mode = M_HOTSPOT;
            drop_ap_us = 0;
            weblog("Wi-Fi: forgot the home network, hotspot only");
        }
        if (pend_ready) apply_requested_change();

        if (sta_ip && mode == M_LOST) {
            mode = M_HOME;
            weblog("Wi-Fi: back on the home network");
        } else if (sta_ip && mode == M_HOTSPOT && have_home &&
                   (ap_clients() == 0 || (drop_ap_us && now >= drop_ap_us))) {
            hotspot_stop();
            mode = M_HOME;
            drop_ap_us = 0;
            char a[16];
            wifi_address(a, sizeof a);
            weblog("Wi-Fi: on the home network, hotspot off [address %s]", a);
        } else if (mode == M_LOST && now - lost_us > A80_HOME_LOST_GRACE_S * 1000000LL) {
            hotspot_start();
            mode = M_HOTSPOT;
            last_try_us = now;
        } else if (mode == M_HOTSPOT && have_home && !sta_ip && !joining && ap_clients() == 0 &&
                   now - last_try_us > A80_HOME_RETRY_S * 1000000LL) {
            last_try_us = now;
            esp_wifi_connect();   // quiet: it would be logged every time
        }
    }
}

void wifi_start(void)
{
    wev = xEventGroupCreate();
    lock = xSemaphoreCreateMutex();
    nvs_load();

    sta_if = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_if, A80_HOSTNAME);
    ap_if = esp_netif_create_default_wifi_ap();   // 192.168.4.1/24 with DHCP server
    // Local-only network: offer no router and no DNS, so iOS doesn't wait on its internet/captive-portal
    // probe (it fails at once) and keeps using cellular for the internet.
    esp_netif_dhcps_stop(ap_if);
    uint8_t off = 0;
    esp_netif_dhcps_option(ap_if, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &off, sizeof off);
    esp_netif_dhcps_option(ap_if, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &off, sizeof off);
    esp_netif_dhcps_start(ap_if);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    if (have_home) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        weblog("Wi-Fi: joining the home network \"%s\" (up to %d s)", home_ssid, A80_HOME_JOIN_S);
        if (try_join(home_ssid, home_pass, A80_HOME_JOIN_S)) {
            mode = M_HOME;
            char a[16];
            wifi_address(a, sizeof a);
            weblog("Wi-Fi: on the home network, no hotspot [address %s]", a);
        } else {
            weblog("Wi-Fi: couldn't join the home network (%s), starting the hotspot and looking again", why_not(last_reason));
            mode = M_HOTSPOT;
            last_try_us = esp_timer_get_time();
            hotspot_start();
        }
    } else {
        mode = M_HOTSPOT;
        hotspot_start();   // sets APSTA mode and the hotspot's settings before the radio starts
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    xTaskCreate(wifi_task, "wifi", 4096, NULL, 3, NULL);   // internal stack: it writes NVS
}

// ---------------------------------------------------------------- web pages
typedef struct { char *p; size_t cap, n; } hb_t;

static void hp(hb_t *b, const char *fmt, ...)
{
    if (b->n >= b->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0) b->n += (size_t)k < b->cap - b->n ? (size_t)k : b->cap - b->n - 1;
}

static void hesc(hb_t *b, const char *s)   // HTML-escape (SSIDs can contain anything)
{
    for (; *s; s++) {
        switch (*s) {
        case '<': hp(b, "&lt;"); break;
        case '>': hp(b, "&gt;"); break;
        case '&': hp(b, "&amp;"); break;
        case '"': hp(b, "&quot;"); break;
        case '\'': hp(b, "&#39;"); break;
        default: if (b->n + 1 < b->cap) { b->p[b->n++] = *s; b->p[b->n] = 0; }
        }
    }
}

static void page_head(hb_t *b, const char *title)
{
    hp(b, "<!doctype html><html><head><meta charset=utf-8>"
          "<meta name=viewport content='width=device-width,initial-scale=1'><title>%s</title><style>"
          "body{font:16px -apple-system,sans-serif;margin:16px;color:#111;background:#fff}"
          "@media(prefers-color-scheme:dark){body{color:#eee;background:#111}input{background:#222;color:#eee}"
          "label{border-color:#333!important}}"
          "h1{font-size:22px;margin:0 0 12px}p{margin:8px 0}.m{color:#888;font-size:14px}"
          "label{display:block;padding:10px 4px;border-bottom:1px solid #ddd}"
          "input[type=text],input[type=password]{width:100%%;box-sizing:border-box;font-size:16px;padding:8px;margin:6px 0}"
          "button{font-size:17px;padding:10px 18px;margin-top:12px}"
          "</style></head><body><h1>%s</h1>", title, title);
}

typedef struct { char ssid[33]; int rssi; bool secure; } net_t;

static int by_signal(const void *a, const void *b)   // strongest first
{
    return ((const net_t *)b)->rssi - ((const net_t *)a)->rssi;
}

// Networks in range, one entry per name, strongest first. Returns the count, or -1 if the radio is busy.
static int scan_networks(net_t *out, int max)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t *r = heap_caps_calloc(n ? n : 1, sizeof *r, MALLOC_CAP_SPIRAM);
    if (!r) return -1;
    esp_wifi_scan_get_ap_records(&n, r);
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *s = (const char *)r[i].ssid;
        if (!s[0]) continue;
        int j = 0;
        while (j < k && strcmp(out[j].ssid, s)) j++;
        if (j == k) {
            if (k == max) continue;
            strlcpy(out[k].ssid, s, sizeof out[k].ssid);
            out[k].rssi = r[i].rssi;
            out[k].secure = r[i].authmode != WIFI_AUTH_OPEN;
            k++;
        } else if (r[i].rssi > out[j].rssi) {   // same name on several access points: keep the strongest
            out[j].rssi = r[i].rssi;
            out[j].secure = r[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    free(r);
    qsort(out, k, sizeof *out, by_signal);
    return k;
}

size_t wifi_describe_html(char *out, size_t cap)
{
    char ssid[33], a[16];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(ssid, home_ssid, sizeof ssid);
    xSemaphoreGive(lock);
    wifi_address(a, sizeof a);
    hb_t b = {out, cap, 0};
    if (cap) out[0] = 0;
    switch (mode) {
    case M_HOME:
        hp(&b, "Home Wi-Fi “"); hesc(&b, ssid); hp(&b, "” (address %s)", a);
        break;
    case M_LOST:
        hp(&b, "Home Wi-Fi “"); hesc(&b, ssid); hp(&b, "” lost, reconnecting");
        break;
    default:
        hp(&b, "Hotspot “%s” (address %s)", A80_AP_SSID, a);
        if (have_home) { hp(&b, "<br>Looking for home Wi-Fi “"); hesc(&b, ssid); hp(&b, "”"); }
    }
    return b.n;
}

size_t wifi_describe_text(char *out, size_t cap)
{
    char ssid[33], a[16];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(ssid, home_ssid, sizeof ssid);
    xSemaphoreGive(lock);
    wifi_address(a, sizeof a);
    int n;
    switch (mode) {
    case M_HOME: n = snprintf(out, cap, "home Wi-Fi \"%s\", address %s", ssid, a); break;
    case M_LOST: n = snprintf(out, cap, "home Wi-Fi \"%s\" lost, reconnecting", ssid); break;
    default:
        n = snprintf(out, cap, "hotspot \"%s\", address %s%s%s%s", A80_AP_SSID, a, have_home ? ", looking for home Wi-Fi \"" : "",
                     have_home ? ssid : "", have_home ? "\"" : "");
    }
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t wifi_setup_html(char *out, size_t cap)
{
    static net_t nets[32];   // one request at a time: the radio can only scan once anyway
    static SemaphoreHandle_t scan_lock;
    if (!scan_lock) scan_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(scan_lock, portMAX_DELAY);
    int k = scan_networks(nets, 32);
    char cur[33];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(cur, home_ssid, sizeof cur);
    xSemaphoreGive(lock);

    hb_t b = {out, cap, 0};
    if (cap) out[0] = 0;
    page_head(&b, "Home Wi-Fi");
    hp(&b, "<p class=m>Now: ");
    if (have_home) { hp(&b, "“"); hesc(&b, cur); hp(&b, "”"); } else hp(&b, "no home network saved (hotspot only)");
    hp(&b, "</p><form method=post action=/wifi><p>Networks in range, strongest first:</p>");
    if (k < 0) hp(&b, "<p>Couldn't scan just now. <a href=/wifi>Try again</a> or type the name below.</p>");
    for (int i = 0; i < k; i++) {
        hp(&b, "<label><input type=radio name=pick value=\"");
        hesc(&b, nets[i].ssid);
        hp(&b, "\"%s> ", i == 0 ? " checked" : "");
        hesc(&b, nets[i].ssid);
        hp(&b, " <span class=m>%s &middot; %s (%d dBm)</span></label>", nets[i].secure ? "password" : "open",
           nets[i].rssi > -55 ? "strong" : nets[i].rssi > -67 ? "good" : nets[i].rssi > -77 ? "fair" : "weak",
           nets[i].rssi);
    }
    if (k == 0) hp(&b, "<p>No networks found. <a href=/wifi>Scan again</a></p>");
    hp(&b, "<p>Or type a name (hidden network):<input type=text name=other autocapitalize=none autocorrect=off></p>"
           "<p>Password (leave empty for an open network):<input type=password name=pass></p>"
           "<button type=submit>Save</button></form>"
           "<p class=m><a href=/wifi>Scan again</a> &middot; <a href=/>Back</a></p>");
    if (have_home)
        hp(&b, "<form method=post action=/wifi onsubmit=\"return confirm('Forget the saved home network?')\">"
               "<input type=hidden name=forget value=1><button type=submit>Forget the saved network</button></form>");
    hp(&b, "</body></html>");
    xSemaphoreGive(scan_lock);
    return b.n;
}

size_t wifi_save_request_html(const char *body, char *out, size_t cap)
{
    hb_t b = {out, cap, 0};
    if (cap) out[0] = 0;
    char flag[4];
    if (form_get(body, "forget", flag, sizeof flag) >= 0) {
        forget_req = true;   // done by the Wi-Fi task within a second
        weblog("Wi-Fi: forgetting the home network was requested from the setup page");
        page_head(&b, "Home Wi-Fi");
        hp(&b, "<p>The saved home network is forgotten. The printer goes back to its own hotspot "
               "\u201c%s\u201d.</p></body></html>", A80_AP_SSID);
        return b.n;
    }

    char pick[40], other[40], pass[72];
    int lp = form_get(body, "pick", pick, sizeof pick);
    int lo = form_get(body, "other", other, sizeof other);
    int lw = form_get(body, "pass", pass, sizeof pass);
    const char *ssid = lo > 0 ? other : pick;
    int ls = lo > 0 ? lo : lp;
    const char *err = NULL;
    if (ls <= 0) err = "Choose a network or type its name.";
    else if (ls > 32) err = "A Wi-Fi name can be at most 32 characters.";
    else if (lw > 63 || (lw > 0 && lw < 8)) err = "A Wi-Fi password is 8 to 63 characters.";

    if (!err && join_state == JR_PENDING) err = "Another change is still being tried. Wait a few seconds and try again.";
    if (err) {
        page_head(&b, "Home Wi-Fi");
        hp(&b, "<p>%s</p><p><a href=/wifi>Back</a></p></body></html>", err);
    } else {
        xSemaphoreTake(lock, portMAX_DELAY);
        strlcpy(pend_ssid, ssid, sizeof pend_ssid);
        strlcpy(pend_pass, lw > 0 ? pass : "", sizeof pend_pass);
        join_state = JR_PENDING;
        pend_ready = true;   // the Wi-Fi task picks it up within a second
        xSemaphoreGive(lock);
        weblog("Wi-Fi: change to \"%s\" requested from the setup page", ssid);

        // Answer with the outcome, while the hotspot is still on: wait for the Wi-Fi task (up to its join time).
        for (int i = 0; i < (A80_HOME_JOIN_S + 5) * 10 && join_state == JR_PENDING; i++) vTaskDelay(pdMS_TO_TICKS(100));
        jstate_t st = join_state;
        if (st == JR_OK) {
            page_head(&b, "Joined");
            hp(&b, "<p>The printer joined \u201c");
            hesc(&b, ssid);
            hp(&b, "\u201d and saved it as the home network.</p><p>Its address there: <b>%s</b><br>"
                   "<a href=\"http://%s/\">http://%s/</a> (open it from a device on \u201c", join_ip, join_ip,
               join_ip);
            hesc(&b, ssid);
            if (ap_on) hp(&b, "\u201d).</p><p>The hotspot turns off in about %d seconds, so this page will lose its connection. "
                              "Join \u201c", A80_SETUP_RESULT_S);
            else hp(&b, "\u201d).</p><p>The printer has left its previous network, so this page will lose its connection. "
                        "Join \u201c");
            hesc(&b, ssid);
            hp(&b, "\u201d on your device to keep printing.</p></body></html>");
        } else if (st == JR_FAIL) {
            page_head(&b, "Couldn't join");
            hp(&b, "<p>The printer couldn't join \u201c");
            hesc(&b, ssid);
            hp(&b, "\u201d: <b>%s</b>.</p><p>Nothing was changed; the hotspot is still on and the previous setting is kept.</p>"
                   "<p><a href=/wifi>Try again</a></p></body></html>", join_why);
        } else {
            page_head(&b, "Still trying");
            hp(&b, "<p>The printer is still trying to join \u201c");
            hesc(&b, ssid);
            hp(&b, "\u201d. <a href=/>Check the status page</a> in a moment.</p></body></html>");
        }
        if (st != JR_PENDING) join_state = JR_IDLE;
    }
    memset(pass, 0, sizeof pass);
    return b.n;
}
