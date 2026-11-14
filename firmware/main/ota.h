#pragma once
#include <stddef.h>
#include <stdint.h>

// Firmware update from the web page (/update). See docs in README.md.
void ota_init(void);   // main task: starts the task that does all flash writes, and marks a new image valid after it ran 30 s

// GET /update: the page. Returns its length.
size_t ota_page_html(char *out, size_t cap);

// An update arrives as one HTTP body. All three calls are made by a web worker; the flash writes happen on the update task
// (flash can't be written from a task whose stack is in PSRAM). On failure *err is a plain-English reason and the update is dropped.
int ota_begin(size_t total, const char **err);
int ota_write(const uint8_t *d, size_t n, const char **err);
int ota_end(const char **err);   // checks the image, makes it the next boot image
void ota_abort(void);
void ota_reboot_soon(void);      // restart 1.5 s from now (after the reply has been sent)
