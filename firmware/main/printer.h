#pragma once
#include <stdbool.h>
#include "urfconv.h"

// BLE link to the A80 + print queue. See PROTOCOL.md for the byte-level details.
void printer_start(void);
// Queue converted pages for printing. Takes ownership of pages[] (and the array).
// The job's IPP state is updated through ipp_job_update().
// fanfold: each page is padded to one fanfold sheet (see config.h) instead of roll feed.
// darkness: printer darkness 0 (Draft) or 2 (Best), or -1 = automatic per page (Normal)
void printer_enqueue(int job_id, a80_page_t **pages, int npages, bool fanfold, int darkness);

// Status page HTML (for GET /). Returns its length.
#include <stddef.h>
size_t printer_status_html(char *out, size_t cap);
