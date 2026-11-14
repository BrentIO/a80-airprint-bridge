#pragma once
#include <stddef.h>

// What the ESP32 can tell about itself: firmware, chip, addresses, memory, flash, restart reason, uptime, Wi-Fi.
// Every value is read when called; nothing the chip can't report is shown.

void espinfo_init(void);   // once, from the main task (reads flash, which other stacks must not)
void espinfo_log(void);                          // the "ESP32 information" block on the console
size_t espinfo_html_rows(char *out, size_t cap);   // the same as <tr> rows for the status page
