// A80 AirPrint bridge - step 1/2: open Wi-Fi hotspot + Bonjour (AirPrint) + IPP server that accepts URF jobs.
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "config.h"
#include "weblog.h"

void httpd_start(void);
#include "espinfo.h"
#include "printer.h"
#include "wifi.h"

static void start_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(A80_HOSTNAME);
    mdns_instance_name_set(A80_PRINTER_NAME);
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"}, {"qtotal", "1"}, {"rp", "ipp/print"}, {"ty", "aixiqee A80"},
        {"product", "(A80)"}, {"note", "Off - press power button"}, {"pdl", "image/urf,image/pwg-raster"}, {"Color", "F"},
        {"Duplex", "F"}, {"Copies", "F"}, {"UUID", A80_UUID}, {"URF", "CP1,IS1,MT1,RS300,V1.4,W8"},
        {"kind", "document,roll"}, {"PaperMax", "legal-A4"}, {"priority", "0"},
        {"adminurl", "http://" A80_HOSTNAME ".local/"},
    };
    ESP_ERROR_CHECK(mdns_service_add(A80_PRINTER_NAME, "_ipp", "_tcp", A80_IPP_PORT, txt,
                                     sizeof txt / sizeof *txt));
    mdns_service_subtype_add_for_host(A80_PRINTER_NAME, "_ipp", "_tcp", NULL, "_universal");
    mdns_service_subtype_add_for_host(A80_PRINTER_NAME, "_ipp", "_tcp", NULL, "_print");
    weblog("Announcing the printer \"%s\" to clients [Bonjour %s.local, _ipp._tcp,_universal, port %d]", A80_PRINTER_NAME, A80_HOSTNAME,
           A80_IPP_PORT);
}

void app_main(void)
{
    weblog_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // ESP-IDF's own Wi-Fi/network chatter is replaced by the plain-English lines above; keep only its warnings.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("mdns_mem", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);
    weblog("A80 AirPrint bridge starting [firmware version %s, commit %s, %u KB PSRAM free]", A80_VERSION, A80_COMMIT,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    wifi_start();
    espinfo_init();
    espinfo_log();   // once at boot too, so it shows even when the printer is off
    start_mdns();
    httpd_start();
    printer_start();
}
