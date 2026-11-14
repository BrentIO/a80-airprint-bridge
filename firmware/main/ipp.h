#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct ipp_txn ipp_txn_t;

// Length of the IPP header (attributes) in d, 0 if more bytes are needed, -1 if malformed.
int ipp_header_len(const uint8_t *d, size_t n);
// Start a request from its header; document bytes (if any) follow via ipp_doc.
ipp_txn_t *ipp_begin(const uint8_t *hdr, size_t len);
int ipp_doc(ipp_txn_t *t, const uint8_t *d, size_t n);   // 0 ok, -1 stop reading
// Finish: build the response (malloc'd, caller frees) and free the transaction.
size_t ipp_finish(ipp_txn_t *t, uint8_t **resp);

void ipp_set_printer_state(int state, const char *reason, const char *message);
void ipp_set_battery(int pct, bool low);   // shown on the iPhone as a supply level
void ipp_set_note(const char *note);       // one-line status shown under the printer's name (location / TXT note)

// Paper mode used when a job doesn't name a media-type: false = roll, true = fanfold. Saved in NVS.
bool ipp_fanfold_default(void);
void ipp_set_fanfold_default(bool fanfold);
void ipp_fanfold_load(void);   // main task only (reads NVS)
void ipp_fanfold_save(void);   // internal-stack task only (writes NVS if changed)
void ipp_settings_start(void); // starts the task that saves the paper default (call once, from the main task)

// Called by the print task.
void ipp_job_update(int id, int state, const char *reasons, int pages_done);   // pages_done < 0: unchanged
bool ipp_job_canceled(int id);
