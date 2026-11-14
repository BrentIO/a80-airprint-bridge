// Tiny HTTP/1.1 server for IPP (POST /ipp/print) and a status page (GET /).
// Handles keep-alive, Expect: 100-continue and chunked request bodies (iOS can send either).
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "config.h"
#include "ipp.h"
#include "printer.h"
#include "wifi.h"
#include "weblog.h"


typedef struct {
    int fd;
    uint8_t buf[2048];
    size_t pos, len;
} conn_t;

static int fill(conn_t *c)
{
    if (c->pos < c->len) return 1;
    int n = recv(c->fd, c->buf, sizeof c->buf, 0);
    if (n <= 0) return 0;
    c->pos = 0;
    c->len = n;
    return 1;
}

// Read one line (without CRLF). Returns length, or -1 on EOF.
static int read_line(conn_t *c, char *out, size_t cap)
{
    size_t n = 0;
    while (fill(c)) {
        char ch = c->buf[c->pos++];
        if (ch == '\n') {
            if (n && out[n - 1] == '\r') n--;
            out[n] = 0;
            return n;
        }
        if (n + 1 < cap) out[n++] = ch;
    }
    return -1;
}

typedef struct { bool chunked, done; long left; } body_t;

// Read up to max body bytes (dechunking). Returns bytes read, 0 at end of body, -1 on error.
static int body_read(conn_t *c, body_t *b, uint8_t *dst, size_t max)
{
    char line[64];
    if (b->done) return 0;
    if (b->chunked && b->left == 0) {
        if (read_line(c, line, sizeof line) < 0) return -1;
        b->left = strtol(line, NULL, 16);
        if (b->left == 0) {
            while (read_line(c, line, sizeof line) > 0) {}   // trailers
            b->done = true;
            return 0;
        }
    }
    if (!b->chunked && b->left == 0) { b->done = true; return 0; }
    if (!fill(c)) return -1;
    size_t k = c->len - c->pos;
    if (k > max) k = max;
    if (k > (size_t)b->left) k = b->left;
    memcpy(dst, c->buf + c->pos, k);
    c->pos += k;
    b->left -= k;
    if (b->chunked && b->left == 0) read_line(c, line, sizeof line);   // CRLF after chunk
    return k;
}

static void send_all(int fd, const void *d, size_t n)
{
    const uint8_t *p = d;
    while (n) {
        int k = send(fd, p, n, 0);
        if (k <= 0) return;
        p += k;
        n -= k;
    }
}

static void respond(int fd, const char *type, const void *body, size_t n)
{
    char hdr[160];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n", type, (unsigned)n);
    send_all(fd, hdr, h);
    send_all(fd, body, n);
}

static void handle_conn(conn_t *c)
{
    struct timeval tv = {.tv_sec = 10};   // idle keep-alive connections free their worker quickly
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char line[512], method[8] = "", path[64] = "";

    int nreq = 0;
    for (;;) {   // keep-alive loop
        if (read_line(c, line, sizeof line) <= 0) {
            if (!nreq) weblog("A connection closed or timed out without sending a complete request [connection %d]", c->fd);
            break;
        }
        nreq++;
        sscanf(line, "%7s %63s", method, path);
        long clen = -1;
        bool chunked = false, expect = false, close_after = false;
        char ctype[48] = "", upgrade[32] = "";
        while (read_line(c, line, sizeof line) > 0) {
            char *v = strchr(line, ':');
            if (!v) continue;
            *v++ = 0;
            while (*v == ' ') v++;
            if (!strcasecmp(line, "Content-Length")) clen = atol(v);
            else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(v, "chunked")) chunked = true;
            else if (!strcasecmp(line, "Expect") && strcasestr(v, "100-continue")) expect = true;
            else if (!strcasecmp(line, "Connection") && strcasestr(v, "close")) close_after = true;
            else if (!strcasecmp(line, "Content-Type")) snprintf(ctype, sizeof ctype, "%s", v);
            else if (!strcasecmp(line, "Upgrade")) snprintf(upgrade, sizeof upgrade, "%s", v);
        }
        // One line per request, so a log shows what a phone or browser really asked for (status page polls included).
        weblog("Request: %s %s [connection %d%s%s%s%s]", method, path, c->fd, ctype[0] ? ", type " : "", ctype,
               upgrade[0] ? ", asks to upgrade to " : "", upgrade);
        if (!strcmp(method, "GET") && !strncmp(path, "/strings/", 9)) {   // names of the paper types for the iPhone
            char cat[512];
            weblog("Client fetched the paper-name catalog [%s, connection %d]", path, c->fd);
            respond(c->fd, "text/strings; charset=utf-8", cat, ipp_strings_en(cat, sizeof cat));
            if (close_after) break;
            continue;
        }
        if (!strcmp(method, "GET")) {
            // /wifi: the home Wi-Fi setup page; anything else: the status page (iOS "Printer Website")
            bool setup = !strncmp(path, "/wifi", 5);
            size_t cap = setup ? 16384 : 12288;
            char *page = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            if (page) {
                respond(c->fd, "text/html; charset=utf-8", page,
                        setup ? wifi_setup_html(page, cap) : printer_status_html(page, cap));
                free(page);
            }
            if (close_after) break;
            continue;
        }
        if (strcmp(method, "POST")) break;
        // While a request body is arriving, allow long pauses: iOS stops sending a document for 10+ s while the
        // printer shows as stopped (2026-11-12: job 2 cut off exactly 10 s in, while job 1 was on hold).
        struct timeval tv_body = {.tv_sec = 120};
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv_body, sizeof tv_body);
        if (expect) send_all(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

        if (!strncmp(path, "/wifi", 5)) {   // the setup form (a few hundred bytes)
            char form[600];
            size_t n = 0;
            body_t fb = {.chunked = chunked, .left = clen > 0 ? clen : 0};
            for (int k; n < sizeof form - 1 && (k = body_read(c, &fb, (uint8_t *)form + n, sizeof form - 1 - n)) > 0;) n += k;
            form[n] = 0;
            char *page = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
            if (page) {
                respond(c->fd, "text/html; charset=utf-8", page, wifi_save_request_html(form, page, 16384));
                free(page);
            }
            setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            if (close_after || !fb.done) break;   // an oversized body leaves bytes unread: drop the connection
            continue;
        }

        // Stream the body: read the IPP header, then feed the document straight into the converter.
        body_t br = {.chunked = chunked, .left = clen > 0 ? clen : 0};
        size_t cap = 64 * 1024, n = 0;
        uint8_t *hdr = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        int hl = 0;
        while (hdr && (hl = ipp_header_len(hdr, n)) == 0 && n < cap) {
            int k = body_read(c, &br, hdr + n, cap - n);
            if (k <= 0) { hl = k < 0 ? -1 : ipp_header_len(hdr, n); break; }
            n += k;
        }
        if (!hdr || hl <= 0) {
            weblog("Client sent a broken or incomplete request; ignored [connection %d, %u bytes]", c->fd, (unsigned)n);
            free(hdr);
            break;
        }
        ipp_txn_t *t = ipp_begin(hdr, hl);
        bool ok = true;
        if (n > (size_t)hl) ok = ipp_doc(t, hdr + hl, n - hl) == 0;
        for (int k; ok && (k = body_read(c, &br, hdr, cap)) != 0;) {
            if (k < 0) { weblog("Upload from the client was cut off part-way [connection %d]", c->fd); ok = false; break; }
            ok = ipp_doc(t, hdr, k) == 0;
        }
        free(hdr);
        uint8_t *resp = NULL;
        size_t rlen = ipp_finish(t, &resp);
        respond(c->fd, "application/ipp", resp, rlen);
        free(resp);
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);   // back to the short idle keep-alive timeout
        if (!ok) break;
        if (close_after) break;
    }
    weblog("Client closed a connection [connection %d]", c->fd);
    close(c->fd);
    free(c);
}

#define WORKERS 8
static QueueHandle_t conn_q;

static void worker_task(void *arg)
{
    for (;;) {
        conn_t *c;
        if (xQueueReceive(conn_q, &c, portMAX_DELAY) == pdTRUE) handle_conn(c);
    }
}

static void server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((int)(intptr_t)arg),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 8);
    weblog("Print server and web pages ready [port %d]", (int)(intptr_t)arg);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        conn_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
        c->fd = fd;
        weblog("Client opened a connection [connection %d from %s, %u KB internal RAM free]", fd, inet_ntoa(peer.sin_addr),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        if (uxQueueSpacesAvailable(conn_q) == 0)
            weblog("Busy: all %d connection handlers in use, this one waits its turn [connection %d]", WORKERS, fd);
        xQueueSend(conn_q, &c, portMAX_DELAY);
    }
}

void httpd_start(void)
{
    conn_q = xQueueCreate(16, sizeof(conn_t *));
    // Worker stacks live in PSRAM to keep internal RAM for Wi-Fi.
    for (int i = 0; i < WORKERS; i++)
        xTaskCreatePinnedToCoreWithCaps(worker_task, "http_worker", 8192, NULL, 5, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreate(server_task, "http_server", 4096, (void *)(intptr_t)A80_IPP_PORT, 5, NULL);
}
