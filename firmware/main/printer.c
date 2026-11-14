// A80 over BLE (NimBLE central). Port of printpdf.ble_send / a80.build_job:
// - connect to the A80 (BLE name "A80_<id>_LE"), use service FF00: write FF02, replies on FF01, credits on FF03 ("01 nn")
// - before each page: status 10 FF 40 must be 00, else hold the job and retry
// - page stream: 128x00, 10 FF F1 03, 1F 80 01 10 (roll), 10 FF 10 00 02 (high), GS v 0 bands, ESC J feed, 10 FF FE 45
// - pushed "ff 01"/"ff 02" during a page => page failed: finish the stream, wait for 00, reprint the page
// - keep-alive: status query every 60 s while idle
// - fanfold mode: pad the page to A80_FANFOLD_ROWS and feed A80_TEAR_DOTS, so each page advances exactly one
//   sheet; running out of paper at the end of a fanfold page is the stack ending, not a failed page
// - paper: a job's media-type picks continuous perforated Letter (fanfold) or the roll; with none, fanfold
#include "espinfo.h"
#include "printer.h"
#include "wifi.h"

#include <stdarg.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "config.h"
#include "ipp.h"
#include "weblog.h"

// The printer's BLE name: A80_BLE_NAME in config.h, or (if empty) the first printer named "A80_<id>_LE".
static bool is_printer_name(const uint8_t *n, int len)
{
    if (A80_BLE_NAME[0]) return len == (int)strlen(A80_BLE_NAME) && !memcmp(n, A80_BLE_NAME, len);
    return len > 7 && !memcmp(n, "A80_", 4) && !memcmp(n + len - 3, "_LE", 3);
}
#define PRINTER_BLE_NAME (A80_BLE_NAME[0] ? A80_BLE_NAME : "A80_*_LE")
#define FEED_MM 4
#define BAND_ROWS 256

// ------------------------------------------------------------ BLE state
static uint8_t own_addr_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t h_ff01, h_ff02, h_ff03, mtu = 23;
static volatile int credits;
static volatile int page_error;   // pushed during a page: 1 = out of paper, 2 = cover open
static EventGroupHandle_t ev;
#define EV_READY BIT0     // connected, discovered, subscribed
#define EV_CREDIT BIT1
#define EV_WRITTEN BIT2
static QueueHandle_t replies;   // FF01 notifications (reply_t)
typedef struct { uint8_t len, d[96]; } reply_t;

static void start_scan(void);
static void refresh_note(void);
// When the printer's link last dropped, on the ESP32's own clock (time since boot). 0 = not since the ESP32 started.
static volatile int64_t last_seen_us;
static char note_now[96] = "Off - press power button";   // the status line, also shown on the web page
static char pinfo[12][2][40];   // printer information (label, value) from the last connection, for the web page
static int npinfo;
static void pinfo_set(const char *label, const char *value)
{
    int i = 0;
    while (i < npinfo && strcmp(pinfo[i][0], label)) i++;
    if (i == npinfo) { if (npinfo == 12) return; npinfo++; }
    snprintf(pinfo[i][0], sizeof pinfo[i][0], "%s", label);
    snprintf(pinfo[i][1], sizeof pinfo[i][1], "%s", value);
}
static int cur_status = -1, cur_battery = -1;   // last status byte / battery % read (for the iPhone status line)
static volatile bool job_active;   // the print task is working on a job (it owns the IPP printer state then)

static const char *status_text(int st, char *b, size_t cap)   // 10 FF 40 bits (PROTOCOL.md)
{
    if (st < 0) return "no answer";
    if (st == 0) return "ready";
    static const char *const bit[] = {"printing", "cover open", "no paper", "low battery", "overheat", "charging?",
                                      "overheat2", "bit7"};
    b[0] = 0;
    for (int i = 0; i < 8; i++)
        if (st & (1 << i)) { if (b[0]) strlcat(b, ", ", cap); strlcat(b, bit[i], cap); }
    return b;
}

static const char *chr_name(uint16_t u)   // what each printer Bluetooth channel is for
{
    switch (u) {
    case 0xFF01: return "printer replies and events";
    case 0xFF02: return "print data to the printer";
    case 0xFF03: return "flow control";
    case 0x2AF0: return "printer replies, alternate";
    case 0x2AF1: return "data to the printer, alternate";
    case 0xFEC8: return "vendor ID service, not used";
    default: return "not used";
    }
}

// Last query sent (10 FF xx ..), so its reply can be decoded in the log.
static volatile uint8_t last_q[4];

// One log line for printer bytes: channel, hex in a fixed-width column (up to 8 bytes, "+" if more), then English.
//   FF01 00 2e                    | battery 46%
// dir: '<' = ESP32 to printer (channel FF02), '>' = printer to ESP32 (FF01 replies/events, FF03 flow control)
static void log_hex(char dir, const char *ch, const uint8_t *d, int n, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void log_hex(char dir, const char *ch, const uint8_t *d, int n, const char *fmt, ...)
{
    char hex[32], text[120];
    int k = 0;
    hex[0] = 0;
    for (int i = 0; i < n && i < 8; i++) k += snprintf(hex + k, sizeof hex - k, i ? " %02X" : "%02X", d[i]);
    if (n > 8) snprintf(hex + k, sizeof hex - k, " +");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    weblog("%c %-4s %-26s| %s", dir, ch, hex, text);
}

// English for a command the ESP32 sends (10 FF ..), for the "<" log lines.
static const char *describe_cmd(const uint8_t *d, int n, char *o, size_t cap)
{
    if (n >= 3 && d[0] == 0x10 && d[1] == 0xFF) {
        switch (d[2]) {
        case 0x40: return "status inquiry";
        case 0x50: return n >= 4 && d[3] == 0xF1 ? "battery inquiry" : "unknown 10 FF 50 inquiry";
        case 0x70: return "printer information inquiry";
        case 0x11: return "darkness setting inquiry";
        case 0x13: return "auto power-off inquiry";
        case 0x12: snprintf(o, cap, "set auto power-off to %d minutes", n >= 5 ? d[3] << 8 | d[4] : -1); return o;
        case 0x20:
            if (n >= 4) switch (d[3]) {
                case 0xF0: return "model inquiry";
                case 0xF1: return "firmware version inquiry";
                case 0xEF: return "boot loader version inquiry";
                case 0xF2: return "serial number inquiry";
            }
            break;
        case 0x30:
            if (n >= 4) return d[3] == 0x11 ? "Bluetooth name inquiry" : d[3] == 0x10 ? "Bluetooth module version inquiry" : "unknown";
            break;
        }
    }
    return "command";
}

// While true, replies on FF01 are not logged by the notify handler: the code that asked logs them itself
// (in English only for the information block, or as one combined status line).
static volatile bool quiet_replies;

// Plain-English description of a notification, for the log.
static void describe_notify(uint16_t attr, const uint8_t *d, int n, char *o, size_t cap)
{
    static const char *const ev_names[] = {"?", "out of paper", "cover open", "overheat", "low battery", "cover closed"};
    char t[80];
    if (attr == h_ff03 && n >= 2 && d[0] == 0x01) snprintf(o, cap, "flow control: printer grants %d more packet(s)", d[1]);
    else if (attr == h_ff03 && n >= 3 && d[0] == 0x02) snprintf(o, cap, "flow control: packet size %d bytes", d[1] | d[2] << 8);
    else if (n == 2 && d[0] == 0xFF) snprintf(o, cap, "printer event: %s", d[1] < 6 ? ev_names[d[1]] : "unknown event");
    else if (n == 2 && d[0] == 0x4F && d[1] == 0x4B) snprintf(o, cap, "OK (job start accepted)");
    else if (n >= 3 && d[0] == 'A' && d[1] == 'T' && d[2] == '+')
        snprintf(o, cap, "printer's Bluetooth chip boot message (AT command to its own processor, harmless): %.*s",
                 n > 2 && d[n - 2] == '\r' ? n - 2 : n, (const char *)d);
    else if (last_q[2] == 0x40 && n == 1) snprintf(o, cap, "status: %s", status_text(d[0], t, sizeof t));
    else if (last_q[2] == 0x50 && last_q[3] == 0xF1 && n == 2) snprintf(o, cap, "battery %d%%", d[1]);
    else if (last_q[2] == 0x11 && n >= 1)
        snprintf(o, cap, "darkness setting: %02x (last used)", d[0]);
    else if (last_q[2] == 0x13 && n == 2) snprintf(o, cap, "auto power-off: %d min", d[0] << 8 | d[1]);
    else if (last_q[2] == 0x70 || last_q[2] == 0x20 || last_q[2] == 0x30) {
        int k = 0;
        for (int i = 0; i < n && k < (int)cap - 1; i++) o[k++] = d[i] >= 32 && d[i] < 127 ? d[i] : '.';
        o[k] = 0;
    } else snprintf(o, cap, "unrecognised reply to %02x %02x %02x", last_q[0], last_q[1], last_q[2]);
}

static int on_notify(uint16_t attr, struct os_mbuf *om)
{
    uint8_t d[128];
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (n > sizeof d) n = sizeof d;
    ble_hs_mbuf_to_flat(om, d, n, NULL);
    // Log every notification in English with its hex, except the per-packet credit grants while printing
    // (hundreds per page; the per-page summary covers them).
    bool grant = attr == h_ff03 && n >= 2 && d[0] == 0x01 && d[1] == 0x01;
    if (!grant) {
        bool event = n == 2 && d[0] == 0xFF;   // pushed events are always logged
        if (!(quiet_replies && attr == h_ff01 && !event)) {
            char text[100];
            describe_notify(attr, d, n, text, sizeof text);
            char ch[8];
            if (attr == h_ff01) strcpy(ch, "FF01");
            else if (attr == h_ff03) strcpy(ch, "FF03");
            else snprintf(ch, sizeof ch, "h%u", attr);
            log_hex('>', ch, d, n, "%s", text);
        }
    }
    if (attr != h_ff03 && n == 2 && d[0] == 0xFF && (d[1] == 0x01 || d[1] == 0x02))   // pushed (FF01 or handle 50)
        page_error = d[1];
    if (attr == h_ff03) {
        if (n >= 2 && d[0] == 0x01) {
            credits += d[1];
            xEventGroupSetBits(ev, EV_CREDIT);
        }
    } else if (attr == h_ff01) {
        reply_t r = {.len = n > 96 ? 96 : n};
        memcpy(r.d, d, r.len);
        xQueueSend(replies, &r, 0);
    }
    return 0;
}

// All characteristics, so every notifying one can be subscribed .
#define MAX_CHR 32
typedef struct { uint16_t def, val, uuid16, cccd; uint8_t props; } chr_t;
static chr_t chrs[MAX_CHR];
static int nchr, dsc_i, sub_i;

static void next_dsc(void);
static void next_sub(void);

static int on_subscribed(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg)
{
    chr_t *ch = &chrs[(intptr_t)arg];
    weblog("Bluetooth: %s messages on channel %04x (%s) [val %u, cccd %u, status %d]",
           e->status ? "could NOT listen for" : "listening for", ch->uuid16, chr_name(ch->uuid16), ch->val, ch->cccd, e->status);
    next_sub();
    return 0;
}

static void next_sub(void)
{
    while (sub_i < nchr) {
        chr_t *ch = &chrs[sub_i++];
        if (!(ch->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) || !ch->cccd) continue;
        if (ch->uuid16 == 0xFEC8) continue;   // Tencent LLSync identity channel: leave alone
        uint8_t v[2] = {ch->props & BLE_GATT_CHR_PROP_NOTIFY ? 1 : 2, 0};
        if (ble_gattc_write_flat(conn, ch->cccd, v, 2, on_subscribed, (void *)(intptr_t)(sub_i - 1)) == 0) return;
    }
    weblog("Bluetooth: printer link ready [MTU %u, %d packets may be sent]", mtu, credits);
    if (!job_active) ipp_set_printer_state(3, "none", "Idle.");
    xEventGroupSetBits(ev, EV_READY);
    cur_status = -1;
    refresh_note();
}

static int on_dsc(uint16_t c, const struct ble_gatt_error *e, uint16_t chr_val, const struct ble_gatt_dsc *d, void *arg)
{
    chr_t *ch = &chrs[(intptr_t)arg];
    if (e->status == 0 && d) {
        if (d->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&d->uuid.u) == 0x2902 && !ch->cccd) ch->cccd = d->handle;
        return 0;
    }
    if (e->status != BLE_HS_EDONE) weblog("Bluetooth: couldn't list the settings of channel %04x [status %d]", ch->uuid16, e->status);
    next_dsc();
    return 0;
}

static void next_dsc(void)
{
    while (dsc_i < nchr) {
        int i = dsc_i++;
        chr_t *ch = &chrs[i];
        if (!(ch->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) continue;
        uint16_t end = i + 1 < nchr ? chrs[i + 1].def - 1 : 0xFFFF;
        if (end <= ch->val) continue;
        if (ble_gattc_disc_all_dscs(conn, ch->val, end, on_dsc, (void *)(intptr_t)i) == 0) return;
    }
    for (int i = 0; i < nchr; i++)
        if (chrs[i].props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))
            weblog("Bluetooth: printer offers channel %04x (%s) [val %u, cccd %u, props %02x]", chrs[i].uuid16,
                   chr_name(chrs[i].uuid16), chrs[i].val, chrs[i].cccd, chrs[i].props);
    credits = 0;
    sub_i = 0;
    next_sub();
}

static int on_chr(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_chr *chr, void *arg)
{
    if (e->status == 0 && chr) {
        if (nchr < MAX_CHR) {
            chr_t *ch = &chrs[nchr++];
            ch->def = chr->def_handle;
            ch->val = chr->val_handle;
            ch->props = chr->properties;
            ch->cccd = 0;
            ch->uuid16 = chr->uuid.u.type == BLE_UUID_TYPE_16 ? ble_uuid_u16(&chr->uuid.u) : 0;
            if (ch->uuid16 == 0xFF01) h_ff01 = ch->val;
            if (ch->uuid16 == 0xFF02) h_ff02 = ch->val;
            if (ch->uuid16 == 0xFF03) h_ff03 = ch->val;
        }
        return 0;
    }
    if (e->status != BLE_HS_EDONE) weblog("Bluetooth: listing the printer's channels failed [status %d]", e->status);
    weblog("Bluetooth: printer has %d channels; %s [FF01=%u FF02=%u FF03=%u]", nchr,
           h_ff01 && h_ff02 && h_ff03 ? "the three needed for printing are present" : "MISSING a channel needed for printing",
           h_ff01, h_ff02, h_ff03);
    if (!h_ff01 || !h_ff02 || !h_ff03) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    dsc_i = 0;
    next_dsc();
    return 0;
}

static int on_mtu(uint16_t c, const struct ble_gatt_error *e, uint16_t m, void *arg)
{
    weblog("Bluetooth: agreed packet size with the printer [MTU %u, status %d]", m, e->status);
    if (m) mtu = m;
    h_ff01 = h_ff02 = h_ff03 = 0;
    nchr = 0;
    ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, on_chr, NULL);
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data)) return 0;
        if (f.name && is_printer_name(f.name, f.name_len)) {
            ble_gap_disc_cancel();
            weblog("Bluetooth: found the printer, connecting [%.*s]", f.name_len, (const char *)f.name);
            ble_gap_connect(own_addr_type, &event->disc.addr, 10000, NULL, gap_event, NULL);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            weblog("Bluetooth: connecting to the printer failed, will retry [status %d]", event->connect.status);
            start_scan();
        } else {
            conn = event->connect.conn_handle;
            weblog("Bluetooth: connected to the printer");
            int rc = ble_gattc_exchange_mtu(conn, on_mtu, NULL);
            if (rc) weblog("Bluetooth: couldn't start packet-size negotiation [rc %d]", rc);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        weblog("Bluetooth: printer disconnected (%s), searching again [reason %d]",
               event->disconnect.reason == 520 ? "link lost: printer switched off, restarted or out of range"
               : event->disconnect.reason == 531 ? "printer ended the connection" : "unexpected", event->disconnect.reason);
        conn = BLE_HS_CONN_HANDLE_NONE;
        if (xEventGroupGetBits(ev) & EV_READY) last_seen_us = esp_timer_get_time();   // it was connected until now
        xEventGroupClearBits(ev, EV_READY);
        // Not an error: the printer powers itself off when idle. iOS shows this before a job is even sent.
        if (!job_active) ipp_set_printer_state(3, "connecting-to-device", "Printer is off - press its power button");
        refresh_note();
        start_scan();
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (conn == BLE_HS_CONN_HANDLE_NONE) start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        return on_notify(event->notify_rx.attr_handle, event->notify_rx.om);
    case BLE_GAP_EVENT_MTU:
        mtu = event->mtu.value;
        return 0;
    }
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {.filter_duplicates = 1, .passive = 0, .itvl = 0x50, .window = 0x30};
    ble_gap_disc(own_addr_type, 30000, &p, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    weblog("Bluetooth: looking for the printer [%s]", PRINTER_BLE_NAME);
    start_scan();
}

static void host_task(void *p)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ------------------------------------------------------------ writing with credits
static int on_written(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg)
{
    if (e->status) weblog("Bluetooth: printer rejected a write [status %d]", e->status);
    xEventGroupSetBits(ev, EV_WRITTEN);
    return 0;
}

static bool write_ack(const uint8_t *d, size_t n)   // write with response
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return false;   // printer not connected: don't log a query that can't be sent
    if (n <= 4) for (int i = 0; i < 4; i++) last_q[i] = i < (int)n ? d[i] : 0;   // a query: remember it for the log
    if (n <= 5 && !quiet_replies) {
        char t[48];
        log_hex('<', "FF02", d, n, "%s", describe_cmd(d, n, t, sizeof t));
    }
    xEventGroupClearBits(ev, EV_WRITTEN);
    if (ble_gattc_write_flat(conn, h_ff02, d, n, on_written, NULL)) return false;
    return xEventGroupWaitBits(ev, EV_WRITTEN, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)) & EV_WRITTEN;
}

typedef struct { uint8_t buf[512]; size_t n, chunk; bool ok; } stream_t;

static int64_t wait_credit_us, wait_ble_us, max_credit_us;   // per page, for the log
static int fallback_acks;

static void flush(stream_t *s)
{
    if (!s->n || !s->ok) return;
    int64_t t0 = esp_timer_get_time();
    while (credits <= 0) {
        if (!(xEventGroupWaitBits(ev, EV_CREDIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)) & EV_CREDIT)) {
            // no credit grant: fall back to one acknowledged write (one acknowledged write)
            fallback_acks++;
            s->ok = write_ack(s->buf, s->n);
            s->n = 0;
            return;
        }
    }
    int64_t t1 = esp_timer_get_time();
    wait_credit_us += t1 - t0;
    if (t1 - t0 > max_credit_us) max_credit_us = t1 - t0;
    credits--;
    int rc;
    while ((rc = ble_gattc_write_no_rsp_flat(conn, h_ff02, s->buf, s->n)) == BLE_HS_ENOMEM) vTaskDelay(1);
    wait_ble_us += esp_timer_get_time() - t1;
    if (rc) s->ok = false;
    s->n = 0;
}

static void out(stream_t *s, const void *d, size_t n)
{
    const uint8_t *p = d;
    while (n && s->ok) {
        size_t k = s->chunk - s->n < n ? s->chunk - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
        if (s->n == s->chunk) flush(s);
    }
}

// ------------------------------------------------------------ printer protocol
static bool printer_low_battery;   // set only from the printer's status flag 08, never from the % reading

// One-line status for the iPhone's printer list. Battery rounded to 5% so the line doesn't change on every wobble.
// "less than a minute ago", "about 5m ago", "about 1h 25m ago", "about 2d 3h ago"; "" if it was never connected.
static void age_text(char *out, size_t cap)
{
    int64_t t = last_seen_us;
    if (!t) { if (cap) out[0] = 0; return; }
    long m = (long)((esp_timer_get_time() - t) / 60000000LL);
    if (m < 1) snprintf(out, cap, "less than a minute ago");
    else if (m < 60) snprintf(out, cap, "about %ldm ago", m);
    else if (m < 1440) snprintf(out, cap, "about %ldh %ldm ago", m / 60, m % 60);
    else snprintf(out, cap, "about %ldd %ldh ago", m / 1440, m / 60 % 24);
}

static void refresh_note(void)
{
    char n[96];
    if (!(xEventGroupGetBits(ev) & EV_READY)) {
        char age[32];
        age_text(age, sizeof age);
        if (age[0]) snprintf(n, sizeof n, "Off - press power button (last connected %s)", age);
        else snprintf(n, sizeof n, "Off - press power button (never connected)");
    } else {
        const char *st = cur_status < 0 ? "On" : cur_status & 0x02 ? "Cover open" : cur_status & 0x04 ? "Out of paper"
                       : cur_status & 0x01 ? "Printing" : "Ready";
        if (cur_battery >= 0) snprintf(n, sizeof n, "%s - battery %d%%%s", st, (cur_battery + 2) / 5 * 5,
                                       printer_low_battery ? " (low)" : "");
        else snprintf(n, sizeof n, "%s", st);
    }
    snprintf(note_now, sizeof note_now, "%s", n);
    ipp_set_note(n);
}

static int query_status(void)   // returns status byte, or -1
{
    reply_t r;
    xQueueReset(replies);
    static const uint8_t q[] = {0x10, 0xFF, 0x40};
    if (!write_ack(q, sizeof q)) return -1;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xQueueReceive(replies, &r, until - xTaskGetTickCount()) == pdTRUE)
        if (r.len == 1) {
            // The printer's own low-battery flag (status bit 08), logged whenever it turns on or off.
            bool low = r.d[0] & 0x08;
            if (low != printer_low_battery)
                weblog(low ? "PRINTER REPORTS LOW BATTERY (its status flag 08 is set)"
                           : "Printer's low-battery flag cleared (status flag 08 off)");
            printer_low_battery = low;
            cur_status = r.d[0];
            refresh_note();
            return r.d[0];
        }
    return -1;
}

static int query_battery(void)   // 10 FF 50 F1 -> "00 nn", nn = percent; -1 if no answer
{
    reply_t r;
    xQueueReset(replies);
    static const uint8_t q[] = {0x10, 0xFF, 0x50, 0xF1};
    if (!write_ack(q, sizeof q)) return -1;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (xQueueReceive(replies, &r, until - xTaskGetTickCount()) == pdTRUE)
        if (r.len == 2 && r.d[0] == 0x00) {
            cur_battery = r.d[1];
            ipp_set_battery(cur_battery, printer_low_battery);
            refresh_note();
            return r.d[1];
        }
    return -1;
}

static const char *status_str(int st)   // print task only (static buffer)
{
    static char b[80];
    return status_text(st, b, sizeof b);
}

static int last_st = -2, last_bat = -1;
static int64_t last_print_us;   // end of the last page: battery reads low for a few minutes after (voltage sag)

static void log_status_battery(const char *when, int st, int bat)   // two hex-first lines
{
    uint8_t sb = st < 0 ? 0xFF : st, bb[2] = {0, bat < 0 ? 0 : bat};
    static const uint8_t qs[] = {0x10, 0xFF, 0x40}, qb[] = {0x10, 0xFF, 0x50, 0xF1};
    log_hex('<', "FF02", qs, 3, "status inquiry");
    if (st < 0) weblog("> %-4s %-26s| %s: printer did not answer", "FF01", "-", when);
    else log_hex('>', "FF01", &sb, 1, "%s: %s", when, status_str(st));
    log_hex('<', "FF02", qb, 4, "battery inquiry");
    if (bat < 0) weblog("> %-4s %-26s| battery: no answer", "FF01", "-");
    else log_hex('>', "FF01", bb, 2, "battery %d%%", bat);
}

static int log_printer(const char *when)   // returns the status byte (-1 = no answer)
{
    quiet_replies = true;
    int st = query_status();
    int bat = query_battery();
    quiet_replies = false;
    log_status_battery(when, st, bat);
    last_st = st;
    if (bat >= 0) last_bat = bat;
    return st;
}

// Raw query: send cmd, return the first reply (any length) in r. false if none within 2 s.
static bool query_raw(const uint8_t *cmd, size_t n, reply_t *r)
{
    xQueueReset(replies);
    if (!write_ack(cmd, n)) return false;
    return xQueueReceive(replies, r, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void log_printer_info(void)   // once per connection: identity, density, auto-off
{
    reply_t r;
    static const uint8_t info[] = {0x10, 0xFF, 0x70}, dens[] = {0x10, 0xFF, 0x11}, off[] = {0x10, 0xFF, 0x13};
    // Read-only identity queries verified on this printer (PROTOCOL.md). Never 10 FF 20 F3: it is a setter.
    static const struct { uint8_t q[4]; const char *label; } ident[] = {
        {{0x10, 0xFF, 0x20, 0xF0}, "Model"},
        {{0x10, 0xFF, 0x20, 0xF1}, "Firmware"},
        {{0x10, 0xFF, 0x20, 0xEF}, "Boot loader version"},
        {{0x10, 0xFF, 0x20, 0xF2}, "Serial number"},
        {{0x10, 0xFF, 0x30, 0x11}, "Bluetooth name"},
        {{0x10, 0xFF, 0x30, 0x10}, "Bluetooth module version"},
    };
    quiet_replies = true;   // English only in this block
    espinfo_log();   // the ESP32 block, directly above the printer block
    weblog("---- Printer information ----");
    for (int i = 0; i < (int)(sizeof ident / sizeof *ident); i++) {
        if (query_raw(ident[i].q, 4, &r) && r.len) {
            weblog("%-26s %.*s", ident[i].label, r.len, r.d);
            char v[40];
            snprintf(v, sizeof v, "%.*s", r.len < 39 ? r.len : 39, r.d);
            pinfo_set(ident[i].label, v);
        }
        else weblog("%-26s (no answer)", ident[i].label);
    }
    if (query_raw(info, sizeof info, &r)) {
        // NAME|MAC|LE MAC|FW|SERIAL|BATT
        char f[6][24] = {{0}};
        int fi = 0, k = 0;
        for (int i = 0; i < r.len && fi < 6; i++) {
            if (r.d[i] == '|') { fi++; k = 0; continue; }
            if (k < 23) f[fi][k++] = r.d[i];
        }
        weblog("%-26s %s", "Name", f[0]);
        weblog("%-26s %s", "MAC address (classic BT)", f[1]);
        weblog("%-26s %s", "MAC address (BT LE)", f[2]);
        weblog("%-26s %s%%", "Battery", f[5]);
        pinfo_set("MAC address (classic BT)", f[1]);
        pinfo_set("MAC address (BT LE)", f[2]);
        cur_battery = atoi(f[5]);
        ipp_set_battery(cur_battery, printer_low_battery);
    }
    if (query_raw(dens, sizeof dens, &r) && r.len >= 1)
        weblog("%-26s %s", "Darkness (last used)", r.d[0] == 0 ? "lightest" : r.d[0] == 1 ? "medium" : r.d[0] == 2 ? "darkest" : "?");
    if (query_raw(off, sizeof off, &r) && r.len >= 2) {
        int min = r.d[0] << 8 | r.d[1];
        weblog("%-26s %d minutes idle", "Auto power-off", min);
        char v[24];
        snprintf(v, sizeof v, "%d min", A80_AUTO_OFF_MIN);
        pinfo_set("Auto power-off", v);
        // The print engine sleeps after this idle time while Bluetooth stays up and keeps answering "ready";
        // status queries don't reset it. Raise it once (10 FF 12 HH LL, Fichero-family setter, user-approved).
        if (min != A80_AUTO_OFF_MIN) {
            static const uint8_t set[] = {0x10, 0xFF, 0x12, A80_AUTO_OFF_MIN >> 8, A80_AUTO_OFF_MIN & 0xFF};
            xQueueReset(replies);
            write_ack(set, sizeof set);
            vTaskDelay(pdMS_TO_TICKS(500));   // any reply is logged by the notify handler
            if (query_raw(off, sizeof off, &r) && r.len >= 2)
                weblog("Changed the printer's auto power-off to %d minutes [read back %d]", A80_AUTO_OFF_MIN, r.d[0] << 8 | r.d[1]);
        }
    }
    int st = query_status();
    quiet_replies = false;
    weblog("%-26s %s", "Status", status_str(st));
    weblog("-----------------------------");
}

// Idle monitor, every 30 s: log status/battery on any change (and every 5 min), and guess charging from the
// battery %, which reads voltage (no charging flag exists): 3 readings in a row that never drop, ending 3+ points
// above the lowest reading,
// or 100% three times in a row, = charging; a fall of more than 5 from the highest while charging = unplugged.
static bool was_ready;   // printer info has been logged for the current connection

static void idle_monitor(void)
{
    static int ticks;
    static bool charging;
    bool ready = xEventGroupGetBits(ev) & EV_READY;
    static int64_t connected_us;
    static bool paused;
    if (ready && !was_ready) { log_printer_info(); connected_us = esp_timer_get_time(); paused = false; }
    was_ready = ready;
    if (!ready) return;
    // Battery-powered printer: stop talking to it 5 min after connecting or printing so its own auto
    // power-off can work. A new print (or reconnect) starts the 5 min again.
    int64_t since = connected_us > last_print_us ? connected_us : last_print_us;
    if (esp_timer_get_time() - since > A80_IDLE_POLL_MIN * 60000000LL) {
        if (!paused) weblog("Idle %d min: status checks paused so the printer can sleep (they resume after the next print)", A80_IDLE_POLL_MIN);
        paused = true;
        return;
    }
    paused = false;
    int prev_st = last_st, prev_bat = last_bat;
    quiet_replies = true;   // the 30-s poll is logged only on change (and every 5 min), below
    int st = query_status();
    int bat = query_battery();
    quiet_replies = false;
    ticks++;
    if (st != prev_st || (bat >= 0 && (bat - prev_bat >= 4 || prev_bat - bat >= 4)) || ticks % 10 == 0) {
        log_status_battery("idle", st, bat);
        last_st = st;
        if (bat >= 0) last_bat = bat;
    }
    if (bat < 0) return;
    // The reading wobbles 2-3 points between polls even when nothing changes (2026-11-12: 26/28/29/26 for
    // 30 min, idle on battery), so compare the lowest of the last 4 readings (2 min): a real charge lifts
    // even the lowest reading; a noisy high reading doesn't.
    static int r[4] = {-1, -1, -1, -1};
    r[0] = r[1]; r[1] = r[2]; r[2] = r[3]; r[3] = bat;
    int floor4 = 101, ceil4 = -1;
    for (int i = 0; i < 4; i++) {
        if (r[i] < 0) { floor4 = -1; break; }
        if (r[i] < floor4) floor4 = r[i];
        if (r[i] > ceil4) ceil4 = r[i];
    }
    static int run_lo = 101, run_hi = -1, full_n;
    full_n = bat >= 100 ? full_n + 1 : 0;
    // For 5 min after printing the reading sags and then rebounds (2026-11-12: 72 -> 49 -> 57 -> ...), which looks
    // like plugging/unplugging: make no charger guesses then, and restart the comparison from the new level.
    if (esp_timer_get_time() - last_print_us < 300000000LL && last_print_us) {
        run_lo = run_hi = bat;
        r[0] = r[1] = r[2] = -1;
        return;
    }
    if (floor4 < 0) return;
    if (floor4 < run_lo) run_lo = floor4;
    if (floor4 > run_hi) run_hi = floor4;
    if (!charging && floor4 - run_lo >= 8) {   // plugging in jumps the reading (13 -> 34 seen); wobble is only 2-3
        charging = true;
        weblog("Battery rising (%d%% -> %d%% for 2 min): charger probably connected", run_lo, floor4);
        run_hi = floor4;
    } else if (!charging && full_n >= 3) {   // full and steady: on the charger
        charging = true;
        weblog("Battery steady at 100%%: charger probably connected");
        run_hi = floor4;
    } else if (charging && run_hi - ceil4 >= 8) {
        charging = false;
        weblog("Battery falling (%d%% -> %d%% for 2 min): charger probably unplugged", run_hi, ceil4);
        run_lo = floor4;
    }
}

static bool send_page(const a80_page_t *p, bool fanfold, int darkness)
{
    int64_t t_page0 = esp_timer_get_time();
    stream_t s = {.chunk = mtu > 3 ? mtu - 3 : 20, .ok = true};
    if (s.chunk > sizeof s.buf) s.chunk = sizeof s.buf;
    static const uint8_t zeros[128];
    const uint8_t head[] = {0x10, 0xFF, 0xF1, 0x03, 0x1F, 0x80, 0x01, 0x10, 0x10, 0xFF, 0x10, 0x00, (uint8_t)darkness};
    out(&s, zeros, sizeof zeros);
    out(&s, head, sizeof head);
    log_hex('<', "FF02", head, sizeof head, "page start: begin job, roll feed, darkness %s (+128 wake-up zeros first)",
            darkness == 0 ? "lightest" : darkness == 1 ? "medium" : "darkest");
    int bpr = p->bytes_per_row;
    // fanfold: exactly A80_FANFOLD_ROWS rows (truncate, or pad with white rows at the bottom)
    int rows = fanfold ? A80_FANFOLD_ROWS : p->rows;
    static const uint8_t blank[256];
    for (int y = 0; y < rows && s.ok; y += BAND_ROWS) {
        int n = rows - y < BAND_ROWS ? rows - y : BAND_ROWS;
        uint8_t gs[8] = {0x1D, 0x76, 0x30, 0x00, bpr & 0xFF, bpr >> 8, n & 0xFF, n >> 8};
        out(&s, gs, 8);
        int have = p->rows - y < 0 ? 0 : p->rows - y < n ? p->rows - y : n;   // real rows in this band
        if (have) out(&s, p->bits + (size_t)y * bpr, (size_t)have * bpr);
        for (size_t k = (size_t)(n - have) * bpr; k && s.ok;) {
            size_t m = k < sizeof blank ? k : sizeof blank;
            out(&s, blank, m);
            k -= m;
        }
        // Per-strip timing, to see where page 1's noisy start comes from: a strip sent with little credit
        // waiting means the printer was waiting on us (buffer filling / possibly running dry); long credit
        // waiting means the printer was the slow one.
        {
            static int64_t band_t0, band_credit0;
            int64_t now = esp_timer_get_time();
            if (y == 0) { band_t0 = t_page0; band_credit0 = 0; }
            size_t black = 0;
            for (size_t k = 0; k < (size_t)have * bpr; k++) black += __builtin_popcount(p->bits[(size_t)y * bpr + k]);
            weblog("  Strip %d of the page (%u%% black) took %.1f s, %s [rows %d-%d, %d ms waiting for printer]",
                   y / BAND_ROWS + 1, (unsigned)(have ? black * 100 / ((size_t)have * bpr * 8) : 0),
                   (now - band_t0) / 1e6, (wait_credit_us - band_credit0) * 10 > (now - band_t0) * 9
                       ? "printer set the pace" : "ESP32 or Bluetooth was the slow part",
                   y, y + n - 1, (int)((wait_credit_us - band_credit0) / 1000));
            band_t0 = now;
            band_credit0 = wait_credit_us;
        }
    }
    for (int feed = fanfold ? A80_TEAR_DOTS : 8 * FEED_MM + 0x80; feed > 0; feed -= 255) {
        uint8_t j[3] = {0x1B, 0x4A, feed > 255 ? 255 : feed};
        out(&s, j, 3);
    }
    static const uint8_t tail[] = {0x10, 0xFF, 0xFE, 0x45};
    out(&s, tail, sizeof tail);
    {
        int feed = fanfold ? A80_TEAR_DOTS : 8 * FEED_MM + 0x80;
        uint8_t t[7] = {0x1B, 0x4A, feed > 255 ? 255 : feed, 0x10, 0xFF, 0xFE, 0x45};
        log_hex('<', "FF02", t, 7, "page end: feed %.1f mm%s, end job", feed / 8.0, fanfold ? " (to the tear bar)" : "");
    }
    flush(&s);
    return s.ok;
}

// ------------------------------------------------------------ queue + print task
typedef struct { int job_id, npages; a80_page_t **pages; bool fanfold; int darkness; } pjob_t;
static QueueHandle_t jobs_q;

void printer_enqueue(int job_id, a80_page_t **pages, int npages, bool fanfold, int darkness)
{
    pjob_t j = {job_id, npages, pages, fanfold, darkness};
    ipp_job_update(job_id, 3, "none", 0);   // pending
    if (xQueueSend(jobs_q, &j, 0) != pdTRUE) {
        weblog("Job %d dropped: the print queue is full", job_id);
        for (int i = 0; i < npages; i++) { free(pages[i]->bits); free(pages[i]); }
        free(pages);
        ipp_job_update(job_id, 8, "resources-are-not-ready", 0);
    }
}

static char last_hold[96];   // the last hold message logged for the current job (logged once, not every 10 s)

static void stopped(int job_id, const char *reason, const char *msg)
{
    if (strcmp(last_hold, msg)) weblog("Job %d on hold: %s", job_id, msg);
    snprintf(last_hold, sizeof last_hold, "%s", msg);
    ipp_set_printer_state(5, reason, msg);
    ipp_job_update(job_id, 6, "printer-stopped", -1);   // processing-stopped
}

static void print_task(void *arg)
{
    for (;;) {
        pjob_t j;
        // idle: keep-alive + monitor every 30 s; check every 1 s while a fresh connection's info is still unlogged
        bool fresh = (xEventGroupGetBits(ev) & EV_READY) && !was_ready;
        if (xQueueReceive(jobs_q, &j, pdMS_TO_TICKS(fresh ? 1000 : 30000)) != pdTRUE) {
            refresh_note();   // while the printer is off the "last connected ..." text moves on (the iPhone sees a change once a minute)
            idle_monitor();
            continue;
        }
        weblog("Printing job %d: %d page(s) on %s, quality %s", j.job_id, j.npages,
               j.fanfold ? A80_PAPER_FANFOLD_NAME : A80_PAPER_ROLL_NAME,
               j.darkness == 0 ? "Draft" : j.darkness == 2 ? "Best" : "Normal (automatic darkness)");
        if (j.fanfold && j.pages[0]->rows > A80_FANFOLD_ROWS)
            weblog("Job %d: the page is %.1f mm taller than a fanfold sheet's printable area; the bottom %.1f mm "
                   "(normally blank margin) is left off [%d rows vs %d]", j.job_id, (j.pages[0]->rows - A80_FANFOLD_ROWS) / 8.0,
                   (j.pages[0]->rows - A80_FANFOLD_ROWS) / 8.0, j.pages[0]->rows, A80_FANFOLD_ROWS);
        int pg = 0;
        bool aborted = false;
        int64_t wait0 = 0;   // when the printer was first found missing for this job
        job_active = true;
        last_hold[0] = 0;
        while (pg < j.npages) {
            if (ipp_job_canceled(j.job_id)) break;
            if (!(xEventGroupWaitBits(ev, EV_READY, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & EV_READY)) {
                // Printer off (it powers itself down when idle). The pages are stored here, so this isn't an
                // error: show "connecting" on the iPhone and wait, up to A80_WAIT_PRINTER_MIN.
                int64_t now = esp_timer_get_time();
                if (!wait0) {
                    char age[32];
                    age_text(age, sizeof age);
                    wait0 = now;
                    weblog("Job %d waiting for the printer: it's off - press its power button (%s%s; the job is stored here; "
                           "waiting up to %d min)", j.job_id, age[0] ? "last connected " : "never connected since the ESP32 started", age,
                           A80_WAIT_PRINTER_MIN);
                }
                refresh_note();
                ipp_set_printer_state(4, "connecting-to-device", "Printer is off - press its power button");
                ipp_job_update(j.job_id, 3, "job-queued", -1);   // pending, not stopped
                if (now - wait0 > A80_WAIT_PRINTER_MIN * 60000000LL) {
                    weblog("Job %d cancelled: the printer wasn't turned on within %d min", j.job_id, A80_WAIT_PRINTER_MIN);
                    ipp_job_update(j.job_id, 8, "aborted-by-system", pg);
                    aborted = true;
                    break;
                }
                continue;
            }
            if (wait0) {
                weblog("Job %d: printer is on, printing now (waited %d s)", j.job_id,
                       (int)((esp_timer_get_time() - wait0) / 1000000));
                wait0 = 0;
            }
            int st = query_status();
            if (st != 0) weblog("Job %d: printer not ready yet: %s [status %02x]", j.job_id, status_str(st), st < 0 ? 0xFF : st);
            if (st != 0) {
                if (st < 0) stopped(j.job_id, "offline-report", "Printer not responding");
                else if (st & 0x02) stopped(j.job_id, "door-open-error", "Cover open");
                else if (st & 0x04) stopped(j.job_id, "media-empty-error", "Out of paper");
                else if (st & 0x01) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }   // still printing
                else stopped(j.job_id, "other-error", "Printer error");
                vTaskDelay(pdMS_TO_TICKS(st == 1 ? 1000 : 5000));
                continue;
            }
            ipp_set_printer_state(4, "none", "Printing");
            ipp_job_update(j.job_id, 5, "job-printing", pg);
            if (pg == 0) log_printer("before job");
            page_error = 0;
            wait_credit_us = wait_ble_us = max_credit_us = 0;
            fallback_acks = 0;
            int64_t t0 = esp_timer_get_time();
            int dk = j.darkness;
            {
                const a80_page_t *p = j.pages[pg];
                size_t nb = (size_t)p->bytes_per_row * p->rows, black = 0;
                for (size_t k = 0; k < nb; k++) black += __builtin_popcount(p->bits[k]);
                int pct = nb ? (int)(black * 100 / (nb * 8)) : 0;
                if (dk < 0) dk = pct > A80_AUTO_DARK_PCT ? 1 : 2;
                weblog("Job %d page %d: sending (%d%% black) at %s darkness%s [darkness %02x]", j.job_id, pg + 1, pct,
                       dk == 0 ? "the lightest" : dk == 1 ? "medium" : "full",
                       j.darkness < 0 ? (dk == 1 ? " (Normal: automatic, page is very dark)" : " (Normal: automatic)")
                                      : j.darkness == 0 ? " (Draft)" : " (Best)", dk);
            }
            bool ok = send_page(j.pages[pg], j.fanfold, dk);
            // Fanfold stacks end at a perforation, i.e. during the feed after a complete page: count the page.
            weblog("Job %d page %d: %s in %.1f s", j.job_id, pg + 1, ok ? "all data sent" : "SENDING FAILED (Bluetooth)",
                   (esp_timer_get_time() - t0) / 1e6);
            bool stack_end = false;
            int pe = 0;   // page_error snapshot, taken once after the page (see below)
            {
                int ms = (int)((esp_timer_get_time() - t0) / 1000);
                weblog("Job %d page %d: average %d KB/s, mostly waiting for the printer [waited %d ms for printer credits "
                       "(longest %d ms), %d ms on BLE buffers, %d fallback acks]", j.job_id, pg + 1, ms ? (int)((long long)j.pages[pg]->bytes_per_row * (j.fanfold ? A80_FANFOLD_ROWS : j.pages[pg]->rows) / ms) : 0,
                       (int)(wait_credit_us / 1000), (int)(max_credit_us / 1000), (int)(wait_ble_us / 1000),
                       fallback_acks);
                int st_after = log_printer("after page");
                // Decide only now: the paper-out event can arrive a second or more after the last byte was sent
                // (2026-11-12 it came 1.5 s later and the page was wrongly reprinted). In fanfold mode, running out
                // of paper during or right after a page always means the stack ended on this page's sheet: the
                // printer keeps printing to the end of the sheet (the sensor fires early), so the page is done.
                pe = page_error;
                stack_end = j.fanfold && (pe == 1 || (st_after > 0 && (st_after & 0x04) && !(st_after & 0x02)));
                if (stack_end) weblog("Job %d page %d: the fanfold stack ran out on this sheet; the page is complete",
                                      j.job_id, pg + 1);
                else if (pe) weblog("Job %d page %d: %s during the page; it will be printed again", j.job_id, pg + 1,
                                    pe == 1 ? "paper ran out" : "cover opened");
                // Asleep print engine: Bluetooth still takes the data (at ~41 KB/s, twice real printing speed) and
                // the printer reports "ready" instead of "printing". Nothing printed, so hold and retry the page.
                long long bytes = (long long)j.pages[pg]->bytes_per_row * (j.fanfold ? A80_FANFOLD_ROWS : j.pages[pg]->rows);
                if (ok && !pe && bytes > 150000 && ms > 0 && bytes / ms > 30 && st_after == 0) {
                    weblog("Job %d page %d: NOT PRINTED - the printer accepted the data but isn't printing (asleep). "
                           "Press the printer's power button; retrying in 30 s [%d KB/s, status ready]", j.job_id, pg + 1,
                           (int)(bytes / ms));
                    stopped(j.job_id, "offline-report", "Printer asleep - press its power button");
                    vTaskDelay(pdMS_TO_TICKS(30000));
                    continue;
                }
            }
            if (ok && (stack_end || !pe)) pg++;
            else vTaskDelay(pdMS_TO_TICKS(2000));
        }
        // wait for the last page to finish printing (status bit 0x01 clears)
        for (int i = 0; i < 60 && query_status() == 1; i++) vTaskDelay(pdMS_TO_TICKS(1000));
        last_print_us = esp_timer_get_time();
        bool canceled = ipp_job_canceled(j.job_id);
        if (!aborted)
            ipp_job_update(j.job_id, canceled ? 7 : 9, canceled ? "job-canceled-by-user" : "job-completed-successfully",
                           pg);
        job_active = false;
        if (xEventGroupGetBits(ev) & EV_READY) ipp_set_printer_state(3, "none", "Idle.");
        else ipp_set_printer_state(3, "connecting-to-device", "Printer is off - press its power button");
        if (!aborted)
            weblog("Job %d %s: %d of %d page(s) printed", j.job_id, canceled ? "cancelled" : "finished", pg, j.npages);
        for (int i = 0; i < j.npages; i++) { free(j.pages[i]->bits); free(j.pages[i]); }
        free(j.pages);
    }
}

void printer_start(void)
{
    weblog("Paper: %s unless a job asks for %s", A80_PAPER_FANFOLD_NAME, A80_PAPER_ROLL_NAME);
    ev = xEventGroupCreate();
    replies = xQueueCreate(16, sizeof(reply_t));
    jobs_q = xQueueCreate(4, sizeof(pjob_t));
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCoreWithCaps(print_task, "print", 6144, NULL, 5, NULL, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Status page for the iPhone ("Printer Website" in the printer's options): what the ESP32 knows right now.
size_t printer_status_html(char *o, size_t cap)
{
    size_t n = 0;
#define P(...) do { int k = snprintf(o + n, n < cap ? cap - n : 0, __VA_ARGS__); if (k > 0) n += k; } while (0)
    bool on = xEventGroupGetBits(ev) & EV_READY;
    refresh_note();   // the "last connected ..." text is current whenever the page is loaded
    char age[32];
    age_text(age, sizeof age);
    P("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<meta http-equiv=refresh content=15><title>%s</title><style>"
      "body{font:16px -apple-system,sans-serif;margin:16px;color:#111;background:#fff}"
      "@media(prefers-color-scheme:dark){body{color:#eee;background:#111}td{border-color:#333!important}}"
      "h1{font-size:22px;margin:0 0 4px}.s{font-size:20px;margin:0 0 16px}"
      "table{border-collapse:collapse;width:100%%}td{padding:7px 4px;border-bottom:1px solid #ddd;vertical-align:top}"
      "td:first-child{color:#888;width:45%%}h2{font-size:15px;margin:22px 0 4px;color:#888;text-transform:uppercase}"
      "</style></head><body><h1>%s</h1><p class=s>%s</p>", A80_PRINTER_NAME, A80_PRINTER_NAME, note_now);
    P("<h2>Printer</h2><table>");
    if (on) P("<tr><td>Connection</td><td>Connected by Bluetooth</td></tr>");
    else P("<tr><td>Connection</td><td>Not connected (off or out of range)</td></tr><tr><td>Last connected</td><td>%s</td></tr>",
           age[0] ? age : "Never connected since the ESP32 started");
    if (cur_battery >= 0)
        P("<tr><td>Battery</td><td>%d%%%s%s</td></tr>", cur_battery, printer_low_battery ? " (printer reports LOW)" : "",
          on ? "" : " (last reading)");
    for (int i = 0; i < npinfo; i++) P("<tr><td>%s</td><td>%s</td></tr>", pinfo[i][0], pinfo[i][1]);
    if (!npinfo) P("<tr><td colspan=2>Details appear once the printer has been switched on.</td></tr>");
    P("</table><h2>ESP32</h2><table>");
    P("<tr><td>Default paper</td><td>%s</td></tr>", A80_PAPER_FANFOLD_NAME);
    if (n < cap) n += espinfo_html_rows(o + n, cap - n);
    P("</table>");
    P("<p style='color:#888;font-size:13px'>Refreshes every 15 s.</p></body></html>");
#undef P
    return n < cap ? n : cap - 1;
}
