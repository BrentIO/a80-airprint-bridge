#pragma once
#include <stdbool.h>
#include <stddef.h>

// Wi-Fi: joins the owner's home network if one is saved, otherwise (or when it can't) runs the "A80-Print"
// hotspot. The setup page (GET/POST /wifi) saves a new home network once the BOOT button confirms it.

void wifi_start(void);   // main task, after esp_netif_init() and the default event loop

// Address a phone should use to reach this bridge now: the home-network address, or the hotspot's.
void wifi_address(char *out, size_t cap);

// One line for the status page: "Home Wi-Fi ...", or the hotspot. HTML, already escaped.
size_t wifi_describe_html(char *out, size_t cap);

// GET /wifi: lists the networks in range (strongest first) with a form. Scans, so it takes a few seconds.
size_t wifi_setup_html(char *out, size_t cap);
// POST /wifi (form body, NUL-terminated): stages the choice until the BOOT button is pressed.
size_t wifi_save_request_html(const char *body, char *out, size_t cap);

// BOOT button: true if the press confirmed a staged Wi-Fi change (so it must not also switch the paper mode).
bool wifi_confirm_pressed(void);
// BOOT button held for 5 s: forget the saved home network and go back to hotspot only.
void wifi_forget(void);
