// Small in-memory log (ring of lines) shown on the status web page, mirrored to the console.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "weblog.h"

#define LINES 80
#define LINE_LEN 160

static char ring[LINES][LINE_LEN];
static int head, count;
static SemaphoreHandle_t mtx;

void weblog_init(void) { mtx = xSemaphoreCreateMutex(); }

void weblog(const char *fmt, ...)
{
    char line[LINE_LEN];
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    int n = snprintf(line, sizeof line, "%02lu:%02lu:%02lu ", (unsigned long)(s / 3600),
                     (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    printf("(%7lu) %s\n", (unsigned long)(esp_timer_get_time() / 1000), line + n);   // no "I ... a80:" prefix
    if (!mtx) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    strncpy(ring[head], line, LINE_LEN - 1);
    ring[head][LINE_LEN - 1] = 0;
    head = (head + 1) % LINES;
    if (count < LINES) count++;
    xSemaphoreGive(mtx);
}

// Copy the log, newest last, as HTML-escaped text into out. Returns length.
size_t weblog_dump(char *out, size_t cap)
{
    size_t len = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        const char *l = ring[(head - count + i + LINES) % LINES];
        for (const char *c = l; *c && len + 8 < cap; c++) {
            if (*c == '<') { memcpy(out + len, "&lt;", 4); len += 4; }
            else if (*c == '&') { memcpy(out + len, "&amp;", 5); len += 5; }
            else out[len++] = *c;
        }
        if (len + 1 < cap) out[len++] = '\n';
    }
    xSemaphoreGive(mtx);
    out[len] = 0;
    return len;
}
