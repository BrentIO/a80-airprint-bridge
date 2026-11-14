#pragma once
#include <stdbool.h>
#include <stddef.h>

// Wi-Fi: joins the owner's home network if one is saved, otherwise (or when it can't) runs the "A80-Print"
// hotspot. The setup page (GET/POST /wifi) saves a new home network once the bridge has really joined it.
// No hardware button is needed for any of it.

void wifi_start(void);   // main task, after esp_netif_init() and the default event loop

// Address a phone should use to reach this bridge now: the home-network address, or the hotspot's.
void wifi_address(char *out, size_t cap);

// One line for the status page: "Home Wi-Fi ...", or the hotspot. HTML, already escaped.
size_t wifi_describe_html(char *out, size_t cap);

// GET /wifi: lists the networks in range (strongest first) with a form. Scans, so it takes a few seconds.
size_t wifi_setup_html(char *out, size_t cap);
// POST /wifi (form body, NUL-terminated): "ssid"/"pick" + "pass" to change the home network (the Wi-Fi task then tries it
// and saves it only if it really joins), or "forget=1" to forget the saved network and go back to hotspot only.
size_t wifi_save_request_html(const char *body, char *out, size_t cap);
