// Firmware update from a web page: the browser sends the "-app" image as the body of POST /update; it is written to the
// other app slot, checked, and made the boot image. A new image that doesn't start falls back to the old one (bootloader
// rollback), because it is only marked valid after it has run for 30 s.
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"
#include "ipp.h"
#include "ota.h"
#include "weblog.h"

typedef enum { C_BEGIN, C_WRITE, C_END, C_ABORT, C_REBOOT } cmd_t;
typedef struct {
    cmd_t cmd;
    const uint8_t *d;
    size_t n;
    const char *err;   // set by the update task when it fails
    int rc;
} req_t;

static QueueHandle_t q;
static SemaphoreHandle_t done, mtx;   // mtx: only one update at a time
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static bool open_;
static size_t written;

static void do_cmd(req_t *r)
{
    r->rc = 0;
    switch (r->cmd) {
    case C_BEGIN:
        target = esp_ota_get_next_update_partition(NULL);
        if (!target) { r->err = "There is no second firmware slot (this board needs the new partition table first)."; r->rc = -1; break; }
        if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) { r->err = "Could not prepare the firmware slot."; r->rc = -1; break; }
        open_ = true;
        written = 0;
        break;
    case C_WRITE:
        if (!open_) { r->err = "No update is in progress."; r->rc = -1; break; }
        if (written == 0 && (r->n < 1 || r->d[0] != 0xE9)) { r->err = "That isn't a firmware image (pick the file ending in -app.bin)."; r->rc = -1; break; }
        if (esp_ota_write(handle, r->d, r->n) != ESP_OK) { r->err = "Writing the firmware failed."; r->rc = -1; break; }
        written += r->n;
        break;
    case C_END: {
        if (!open_) { r->err = "No update is in progress."; r->rc = -1; break; }
        open_ = false;
        if (esp_ota_end(handle) != ESP_OK) { r->err = "The image is damaged or incomplete (nothing was changed)."; r->rc = -1; break; }
        esp_app_desc_t d;
        const esp_app_desc_t *me = esp_app_get_description();
        if (esp_ota_get_partition_description(target, &d) != ESP_OK || strcmp(d.project_name, me->project_name)) {
            r->err = "That image is not firmware for this printer bridge (nothing was changed).";
            r->rc = -1;
            break;
        }
        if (esp_ota_set_boot_partition(target) != ESP_OK) { r->err = "Could not select the new firmware."; r->rc = -1; break; }
        weblog("Firmware update: version %s (%u KB) written to slot %s; restarting", d.version, (unsigned)(written / 1024), target->label);
        break;
    }
    case C_ABORT:
        if (open_) { esp_ota_abort(handle); open_ = false; }
        break;
    case C_REBOOT:
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        break;
    }
}

static void ota_task(void *arg)
{
    int64_t t0 = 0;
    for (;;) {
        req_t *r;
        // Wake up every second to mark the running image valid once it has run for 30 s.
        if (xQueueReceive(q, &r, pdMS_TO_TICKS(1000)) == pdTRUE) {
            do_cmd(r);
            if (r->cmd != C_REBOOT) xSemaphoreGive(done);
        }
        if (!t0) t0 = xTaskGetTickCount();
        else if (t0 > 0 && (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(30000)) {
            esp_ota_mark_app_valid_cancel_rollback();
            t0 = -1;
        }
    }
}

void ota_init(void)
{
    q = xQueueCreate(2, sizeof(req_t *));
    done = xSemaphoreCreateBinary();
    mtx = xSemaphoreCreateMutex();
    xTaskCreate(ota_task, "ota", 6144, NULL, 4, NULL);   // internal-RAM stack: flash writes need it
}

static int call(cmd_t cmd, const uint8_t *d, size_t n, const char **err)
{
    req_t r = {.cmd = cmd, .d = d, .n = n};
    req_t *p = &r;
    xQueueSend(q, &p, portMAX_DELAY);
    xSemaphoreTake(done, portMAX_DELAY);
    if (r.rc && err) *err = r.err;
    return r.rc;
}

int ota_begin(size_t total, const char **err)
{
    if (ipp_busy()) { *err = "A print job is in progress. Wait for it to finish, then try again."; return -1; }
    if (total < 64 * 1024 || total > 0x170000) { *err = "That file is the wrong size for a firmware image."; return -1; }
    if (xSemaphoreTake(mtx, 0) != pdTRUE) { *err = "Another update is already in progress."; return -1; }
    if (call(C_BEGIN, NULL, 0, err)) { xSemaphoreGive(mtx); return -1; }
    return 0;
}

int ota_write(const uint8_t *d, size_t n, const char **err) { return call(C_WRITE, d, n, err); }

int ota_end(const char **err)
{
    int rc = call(C_END, NULL, 0, err);
    xSemaphoreGive(mtx);
    return rc;
}

void ota_abort(void)
{
    call(C_ABORT, NULL, 0, NULL);
    xSemaphoreGive(mtx);
}

void ota_reboot_soon(void)
{
    static req_t r = {.cmd = C_REBOOT};
    req_t *p = &r;
    xQueueSend(q, &p, portMAX_DELAY);
}

size_t ota_page_html(char *out, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return snprintf(out, cap,
        "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Update firmware</title><style>body{font:16px -apple-system,sans-serif;margin:16px;max-width:34em}"
        "button{font-size:1em;padding:.5em 1em}progress{width:100%%}#msg{margin-top:1em}</style></head><body>"
        "<h2>Update firmware</h2>"
        "<p>Running: version %s, commit %s (slot %s).</p>"
        "<p>Pick the update file, the one ending in <b>-app.bin</b>. Keep the phone close to the printer's Wi-Fi and "
        "don't switch anything off until this page says it is done. Your saved Wi-Fi is kept.</p>"
        "<input type=file id=f accept='.bin'><p><button id=b>Update</button></p><progress id=p value=0 max=100 hidden></progress>"
        "<div id=msg></div><p><a href='/'>Back to the status page</a></p>"
        "<script>var f=document.getElementById('f'),b=document.getElementById('b'),p=document.getElementById('p'),m=document.getElementById('msg');"
        "b.onclick=function(){if(!f.files.length){m.textContent='Pick the -app.bin file first.';return}"
        "b.disabled=true;p.hidden=false;m.textContent='Sending...';var x=new XMLHttpRequest();x.open('POST','/update');"
        "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=100*e.loaded/e.total};"
        "x.onload=function(){m.textContent=x.responseText;if(x.status==200){m.textContent+=' The printer is restarting; this page reloads in 25 s.';"
        "setTimeout(function(){location.href='/'},25000)}else b.disabled=false};"
        "x.onerror=function(){m.textContent='The connection was lost. Check the printer, then try again.';b.disabled=false};"
        "x.send(f.files[0])};</script></body></html>",
        A80_VERSION, A80_COMMIT, run ? run->label : "?");
}
