// ESP32's own information, as a console block and as status-page rows (one list drives both).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "config.h"
#include "espinfo.h"
#include "weblog.h"
#include "wifi.h"

#define MAXITEMS 24
typedef struct { char label[32], value[112]; } item_t;

static void put(item_t *it, int *n, const char *label, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(item_t *it, int *n, const char *label, const char *fmt, ...)
{
    if (*n >= MAXITEMS) return;
    strlcpy(it[*n].label, label, sizeof it[*n].label);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it[*n].value, sizeof it[*n].value, fmt, ap);
    va_end(ap);
    (*n)++;
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "woke from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dipped)";
    case ESP_RST_USB: return "USB reset";
    default: return NULL;   // unknown: not shown
    }
}

static void mem_line(item_t *it, int *n, const char *label, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    if (!total) return;
    put(it, n, label, "%u KB free of %u KB (lowest %u KB, largest block %u KB)", (unsigned)(heap_caps_get_free_size(caps) / 1024),
        (unsigned)(total / 1024), (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(caps) / 1024));
}

static void mac_line(item_t *it, int *n, const char *label, esp_mac_type_t t)
{
    uint8_t m[6];
    if (esp_read_mac(m, t) == ESP_OK) put(it, n, label, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Reading flash from a task whose stack is in PSRAM trips an ESP-IDF assert (the cache is off during the read), and the
// web workers and the printer task are such tasks. So the flash-derived values are read once at boot, from the main
// task, and only the saved text is used afterwards.
static char flash_txt[16], space_txt[64];

void espinfo_init(void)
{
    uint32_t fsize;
    if (esp_flash_get_size(NULL, &fsize) == ESP_OK) snprintf(flash_txt, sizeof flash_txt, "%u MB", (unsigned)(fsize / (1024 * 1024)));
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return;
    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK)
        snprintf(space_txt, sizeof space_txt, "%u KB used of %u KB (%u%%)", (unsigned)(meta.image_len / 1024), (unsigned)(p->size / 1024),
                 (unsigned)(meta.image_len * 100ull / p->size));
    else snprintf(space_txt, sizeof space_txt, "%u KB partition", (unsigned)(p->size / 1024));
}

static int collect(item_t *it)
{
    int n = 0;
    const esp_app_desc_t *app = esp_app_get_description();
    put(it, &n, "Firmware", "version %s, commit %s", A80_VERSION, A80_COMMIT);
    put(it, &n, "Built", "%s %s", app->date, app->time);
    put(it, &n, "ESP-IDF", "%s", app->idf_ver);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const char *model = ci.model == CHIP_ESP32S3 ? "ESP32-S3" : ci.model == CHIP_ESP32 ? "ESP32" : ci.model == CHIP_ESP32S2 ? "ESP32-S2"
                      : ci.model == CHIP_ESP32C3 ? "ESP32-C3" : NULL;
    char feat[80] = "";
    if (ci.features & CHIP_FEATURE_WIFI_BGN) strlcat(feat, "Wi-Fi, ", sizeof feat);
    if (ci.features & CHIP_FEATURE_BLE) strlcat(feat, "Bluetooth LE, ", sizeof feat);
    if (ci.features & CHIP_FEATURE_BT) strlcat(feat, "Bluetooth classic, ", sizeof feat);
    if (ci.features & CHIP_FEATURE_EMB_FLASH) strlcat(feat, "embedded flash, ", sizeof feat);
    if (ci.features & CHIP_FEATURE_EMB_PSRAM) strlcat(feat, "embedded PSRAM, ", sizeof feat);
    size_t fl = strlen(feat);
    if (fl > 1) feat[fl - 2] = 0;
    put(it, &n, "Chip", "%s, revision %d.%d, %d core%s, %d MHz", model ? model : "(unknown model)", ci.revision / 100, ci.revision % 100,
        ci.cores, ci.cores == 1 ? "" : "s", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    if (feat[0]) put(it, &n, "Chip features", "%s", feat);

    mac_line(it, &n, "MAC address (Wi-Fi station)", ESP_MAC_WIFI_STA);
    mac_line(it, &n, "MAC address (Wi-Fi hotspot)", ESP_MAC_WIFI_SOFTAP);
    mac_line(it, &n, "MAC address (Bluetooth)", ESP_MAC_BT);

    mem_line(it, &n, "Internal RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    mem_line(it, &n, "PSRAM", MALLOC_CAP_SPIRAM);

    if (flash_txt[0]) put(it, &n, "Flash", "%s", flash_txt);
    if (space_txt[0]) put(it, &n, "Firmware space", "%s", space_txt);

    const char *rr = reset_text(esp_reset_reason());
    if (rr) put(it, &n, "Last restart", "%s", rr);
    int64_t up = esp_timer_get_time() / 1000000;
    put(it, &n, "Up for", "%dh %02dm %02ds", (int)(up / 3600), (int)(up / 60 % 60), (int)(up % 60));
    char w[120];
    wifi_describe_text(w, sizeof w);
    put(it, &n, "Wi-Fi", "%s", w);
    return n;
}

void espinfo_log(void)
{
    item_t *it = malloc(MAXITEMS * sizeof *it);   // heap, not stack: the callers' stacks are small
    if (!it) return;
    int n = collect(it);
    weblog("---- ESP32 information ----");
    for (int i = 0; i < n; i++) weblog("%-28s %s", it[i].label, it[i].value);
    free(it);
}

size_t espinfo_html_rows(char *o, size_t cap)
{
    item_t *it = malloc(MAXITEMS * sizeof *it);
    if (!it) return 0;
    int n = collect(it);
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(o + k, k < cap ? cap - k : 0, "<tr><td>%s</td><td>", it[i].label);
        if (w > 0) k += w;
        // escape the value (the home network name can contain anything)
        for (const char *s = it[i].value; *s; s++) {
            const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
            if (r) w = snprintf(o + k, k < cap ? cap - k : 0, "%s", r);
            else w = snprintf(o + k, k < cap ? cap - k : 0, "%c", *s);
            if (w > 0) k += w;
        }
        w = snprintf(o + k, k < cap ? cap - k : 0, "%s</td></tr>", i == n - 1 ? "<br><a href=/wifi>Change Wi-Fi</a>" : "");
        if (w > 0) k += w;
    }
    free(it);
    return k < cap ? k : (cap ? cap - 1 : 0);
}
