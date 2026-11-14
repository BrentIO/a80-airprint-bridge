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
#include "printer.h"

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = data;
        weblog("Wi-Fi: gave the phone address " IPSTR, IP2STR(&e->ip));
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        weblog("Wi-Fi: a phone joined the hotspot [MAC %02x:%02x:%02x:%02x:%02x:%02x]", e->mac[0], e->mac[1], e->mac[2],
               e->mac[3], e->mac[4], e->mac[5]);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        weblog("Wi-Fi: a phone left the hotspot");
    }
}

static void start_ap(void)
{
    esp_netif_t *ap_if = esp_netif_create_default_wifi_ap();   // 192.168.4.1/24 with DHCP server
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
    wifi_config_t ap = {.ap = {.ssid = A80_AP_SSID, .ssid_len = strlen(A80_AP_SSID),
                               .channel = A80_AP_CHANNEL, .max_connection = A80_AP_MAX_CONN,
                               .authmode = WIFI_AUTH_OPEN}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    weblog("Wi-Fi hotspot \"%s\" is on (no password) [address %s]", A80_AP_SSID, A80_AP_IP);
}

static void start_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(A80_HOSTNAME);
    mdns_instance_name_set(A80_PRINTER_NAME);
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"}, {"qtotal", "1"}, {"rp", "ipp/print"}, {"ty", "aixiqee A80"},
        {"product", "(A80)"}, {"note", "Off - press power button"}, {"pdl", "image/urf"}, {"Color", "F"},
        {"Duplex", "F"}, {"Copies", "F"}, {"UUID", A80_UUID}, {"URF", "CP1,IS1,MT1,RS300,V1.4,W8"},
        {"kind", "document,roll"}, {"PaperMax", "legal-A4"}, {"priority", "0"},
        {"adminurl", "http://" A80_AP_IP "/"},
    };
    ESP_ERROR_CHECK(mdns_service_add(A80_PRINTER_NAME, "_ipp", "_tcp", A80_IPP_PORT, txt,
                                     sizeof txt / sizeof *txt));
    mdns_service_subtype_add_for_host(A80_PRINTER_NAME, "_ipp", "_tcp", NULL, "_universal");
    mdns_service_subtype_add_for_host(A80_PRINTER_NAME, "_ipp", "_tcp", NULL, "_print");
    weblog("Announcing the printer \"%s\" to iPhones [Bonjour %s.local, _ipp._tcp,_universal, port %d]", A80_PRINTER_NAME, A80_HOSTNAME,
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
    weblog("A80 AirPrint bridge starting [%u KB PSRAM free]",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    start_ap();
    start_mdns();
    httpd_start();
    printer_start();
}
