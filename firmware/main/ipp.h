#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct ipp_txn ipp_txn_t;

// Length of the IPP header (attributes) in d, 0 if more bytes are needed, -1 if malformed.
int ipp_header_len(const uint8_t *d, size_t n);
// Start a request from its header; document bytes (if any) follow via ipp_doc.
ipp_txn_t *ipp_begin(const uint8_t *hdr, size_t len, const char *client_ip);
int ipp_doc(ipp_txn_t *t, const uint8_t *d, size_t n);   // 0 ok, -1 stop reading
// Finish: build the response (malloc'd, caller frees) and free the transaction.
size_t ipp_finish(ipp_txn_t *t, uint8_t **resp);

void ipp_set_printer_state(int state, const char *reason, const char *message);
void ipp_set_battery(int pct, bool low);   // shown on the iPhone as a supply level
void ipp_set_note(const char *note);       // one-line status shown under the printer's name (location / TXT note)

// Message catalog (text/strings) naming the paper types on the iPhone, served at /strings/en.strings.
size_t ipp_strings_en(char *out, size_t cap);

// Called by the print task.
void ipp_job_update(int id, int state, const char *reasons, int pages_done);   // pages_done < 0: unchanged
bool ipp_job_canceled(int id);

// True while a job is waiting, receiving or printing (a firmware update must not start then).
bool ipp_busy(void);
