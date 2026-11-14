# A80 AirPrint bridge: ESP32-S3 firmware

iPhone → (Wi-Fi hotspot, AirPrint/IPP, URF) → ESP32-S3 → (Bluetooth LE) → A80 thermal printer.
The AirPrint side is described in `../docs/AIRPRINT.md`; the printer protocol in `../PROTOCOL.md`.

## Status (2026-11-12, evening)

**Current: step30** = step29 (verified on hardware 2026-11-13) + the printer is found by name pattern `A80_<id>_LE` (or `A80_BLE_NAME` in `config.h`). iOS shows: the status line under the printer name (Known Printers), the battery under Printer Info → Supply Levels, and the ESP32 status page under "Show Printer Web Page". A job sent while the printer is off waits quietly and prints when it is switched on.

Working and verified on hardware:
- iPhone → AirPrint over the ESP32's open hotspot `A80-Print` → BLE → A80, roll and fanfold Letter paper.
- Fanfold: exact page step, no reprint when the stack runs out on the last sheet.
- Quality: Normal = automatic darkness (01 for pages > 50% black, else 02), Best = 02. Draft isn't advertised;
  iOS may still show it, and it prints as Normal.
- Printer info block on connect (model, firmware, boot loader, serial, BT name/module, MACs, battery, darkness,
  auto-off, status), hex-first log with `<` (ESP32 → printer) and `>` (printer → ESP32) lines.
- **Sleep:** the ESP32 stops polling 5 min after connecting or printing; the printer's auto power-off is set to 5 min,
  so it turns off ~10 min after the last activity (confirmed). Jobs sent while it's off are held with
  "Printer not connected" until the power button is pressed.
- "Printer asleep" detection (page accepted at > 30 KB/s and status `00` after = not printed: hold + retry).

Step history (each image = previous + the change):

| Step | Change |
|---|---|
| 3–7 | BLE printing; fanfold mode (`continuous-short`), page-step calibration; DHCP without router/DNS; diagnostics |
| 8–11 | Idle monitor, printer info, charging guess, English notifications, asleep detection, auto-off 60 |
| 12–16 | Per-strip timing; stack-end fix; quality → darkness; Normal = automatic |
| 17–21 | Draft removed, fanfold step −5 dots; plain-English hex-first log with `<`/`>` lines; Draft prints as Normal |
| 22 | Charging guess uses the lowest of the last 4 readings, needs a rise or fall of 8+; battery logged on 4+ point changes |
| 23 | Polling stops 5 min after connecting or printing (`A80_IDLE_POLL_MIN`) so the printer can sleep |
| 24 | Auto power-off set to 5 min (`A80_AUTO_OFF_MIN`) |
| 25 | The ESP32's own "Battery LOW (20%)" warning removed; only the printer's status flag `08` is reported |
| 30 | Printer found by BLE name pattern `A80_<id>_LE`, or the exact `A80_BLE_NAME` in `config.h` (was hard-coded to one unit). Builds; not yet run on hardware |
| 29 | Printer website (iOS printer options): `printer-more-info` was port 80, where nothing listens; now `http://192.168.4.1:8631/`, which serves an HTML status page (status line, connection, battery, model, firmware, boot loader, serial, BT name/module, MACs, auto-off; paper mode, uptime, bridge version), auto-refresh 15 s. Battery supply now described as black toner (`#000000`, `toner`) (step28's type `other` showed "Information Not Available"). **Verified 2026-11-13:** Supply Levels shows the battery, web page opens |
| 28 | iPhone display: battery reported as a supply level (`marker-*`, `printer-supply`; "low" only from the printer's flag 08 → `marker-supply-low-warning`); one-line status ("Ready - battery 80%", "Off - press power button", "Out of paper", …) as `printer-location` and the Bonjour TXT `note`, updated on change (battery rounded to 5%). **Verified:** shown under the printer's name in iOS Known Printers |
| 27 | Printer off = not an error: iOS sees printer-state-reasons `connecting-to-device` and "Printer is off - press its power button" (also before a job is sent, and at boot); a waiting job is pending (`job-queued`), not stopped; logged once; prints when the printer comes on; cancelled after 30 min (`A80_WAIT_PRINTER_MIN`). Before: iOS flashed an error dialog ~10 s after the upload |
| 26 | IPP upload timeout 120 s while a body arrives (iOS pauses uploads while the printer shows stopped; job 2 was cut off at exactly 10 s); no log line for a query when the printer isn't connected |

## Home Wi-Fi (issue #4)

`main/wifi.c` owns Wi-Fi. Modes: `M_HOME` (station only), `M_LOST` (home dropped, reconnecting for 20 s) and
`M_HOTSPOT` (hotspot up, home retried every 45 s while no phone is on it). The settings are in `config.h`
(`A80_HOME_*`). The saved network lives in NVS namespace `wifi` (keys `ssid`, `pass`), read in the main task and
written by the Wi-Fi task (internal stack; PSRAM-stack tasks must not touch NVS). A new network is saved only after a
successful join. The setup page is `GET/POST /wifi` (served by `httpd.c`): the POST stages the choice (or `forget=1`), the Wi-Fi
task acts on it within a second, and the POST waits for the outcome (`join_state`) and answers with the new address
or the error, while the hotspot is still on (`A80_SETUP_RESULT_S`). The form is parsed by `formparse.h`, which has a host test
(`test/form_host_test.c`, run by CI). No button is involved. Not yet verified on hardware: joining a real network,
hotspot fallback timing, Bonjour on the home network, and printing over it.

## Roadmap

- Verify fanfold page step `A80_FANFOLD_FIX` = 27 over longer runs (the perforation should stay at the tear bar).
- **Error path test** (cover open / paper out mid-job → iOS alert → resume).
- iOS **PDF print preview** doesn't render with the ESP32 (it did with an earlier Mac-based prototype).
- Roll mode: trim trailing blank rows.

## History: dark-page noise (resolved)

**RESOLVED 2026-11-12 (step16): inconsistent quality on dark pages.** Cold-head dark photo at Normal (automatic ->
   printer darkness 01): quiet, print looks fine, 35 s (strips 2.7/6.1/5.7/3.9/4.2/4.1/3.7/3.5 s) vs 49 s and a
   stuttering first 2-3 inches at Best. History of the investigation: Pattern seen 3 times with the
   same 71%-black photo: page 1 noisy and marginal for roughly its first third, page 2 much cleaner.
   Data: job 3 (on charger, battery low-ish) page 1 took 43 s at 10 KB/s, page 2 30 s at 14 KB/s; the earlier
   step6 job: 46 s / 32 s / 29 s. Time went to waiting for printer credits, so the printer itself slowed down;
   the ESP32 was not starving it. Hypotheses to test, cheapest first (use roll paper):
   - Battery: **ruled out 2026-11-12**: fully charged + on charger, first 2-3 inches of page 1 still poor.
   - Leading hypothesis: page 1 starts with the printer's buffer empty, so it prints as fast as BLE delivers and
     stutters; page 2 starts with a full buffer. 2-3 inches ≈ 400-600 rows ≈ 80-120 KB ≈ the buffer size.
     step12's strip log tells: small credit waits in the first strips = printer starved by us.
     **Result 2026-11-12: NOT starvation.** Every strip was ~98% waiting on printer credits. Same 71%-black photo,
     strip times (32 mm each): cold print 3.6 / **8.5 / 7.4** / 6.1 / 6.1 / 5.2 / 5.1 / 5.2 s (noisy first 2-3 in);
     same page again 80 s later (warm) 3.2 / 6.4 / 5.8 / 4.9 / 4.5 / 4.3 s (fine). An 8%-black recipe page:
     ~3.4 s per strip. So the printer itself slows for dense content, more so with a cold head, and gets noisy
     below ~35 rows/s. A typical text page is fine; the dark photo is the outlier, and its noise is a recent onset
     (it printed fine before). Next options: Normal quality automatically for very dark pages (> ~50% black), or the printer's
     speed setter `10 FF C0 nn` (a printer setting change).
     **Darkness test 2026-11-12 (same photo, strip 2 / strip 3 / whole page):** Best (cold) 8.5 / 7.4 / 49 s,
     Normal (head partly warm) 6.0 / 4.6 / 33 s, Draft (cold) 4.7 / 4.3 / 30 s. By ear: Draft had only 3-4 lines
     of stutter, Normal noticeably noisier. Darkness clearly drives the slowdown. Image quality at Normal/Draft not
     yet judged (the test reused one blackened sheet). step13's stack-end fix verified twice (no reprint).
   - Cold head: the printer may need more energy per line until the head warms up, then slows/stutters.
     Try a short warm-up (a few blank or light rows, or pre-feed) before dense pages, or print dense pages at
     medium darkness (`10 FF 10 00 01`) and compare.
   - Darkness by content: pick medium automatically when a page is very dark (e.g. > 50% black), high otherwise.
   - Steady pacing: cap the send rate to the printer's sustained speed so its buffer never runs dry
     mid-page (stop-start noise), instead of letting it fill and drain.
   Log `job N page M: … KB/s; waited … for printer credits` for each test and compare page 1 vs page 2.

## Key findings 2026-11-11 (details in ../PROTOCOL.md)

- BLE printing from iPhone works (step3+). Fanfold mode works: no mark sensor; exact page step =
  2142 printed rows + 120-dot feed (config.h, step17), drift ≈ 0 over 3 pages; load by inserting (parks ~92 mm out), then
  open the cover and slide the perforation back to the tear bar. Last sheet of a stack can't feed out (paper sensor
  ~7 mm before the tear bar).
- iOS: paper type is on the "Media & Quality" tab; `continuous-short` = fanfold = **Continuous Perforated Letter**
  (the default, also for jobs that name no type) and `stationery` = **Paper Roll**. The two names come from a
  PWG 5100.13 message catalog the printer serves at `/strings/en.strings`, but iOS fetches it and ignores it for
  media types (tested 2026-11-14), so the types are advertised with our own keywords (`a80-continuous-perforated-letter`, `a80-paper-roll`) that the
  catalog names (`A80_MEDIA_TYPE_NAMES` in `config.h`: 0 standard keywords, 1 free-text names, 2 our keywords).
  Apple's CUPS looks a value up in the printer's catalog first and shows free text as "Other Media", which is why
  option 2. Whether iOS uses the catalog for keywords it has no built-in name for is checked on the phone.
  Copies arrive as one multi-page job. iOS polls only page counts, so progress is per page (no % within a page).
- Hotspot joins are slow-ish because there is no internet; DHCP now offers no router/DNS (step5).
- No charging indicator exists (re-probed read-only via every GATT characteristic); charging is inferred from the
  battery % (lowest of 4 readings rising 8+), or 100% steady.
- Printer can silently discard jobs while still answering "ready" (asleep or stuck); a printer power-cycle fixed it.
  Signature: page accepted at ~41 KB/s and status `00` (not `01`) right after.

## Hardware

- **ESP32-S3-WROOM-1 N8R8** (8 MB flash, 8 MB octal PSRAM). The esptool report says "Embedded PSRAM 8MB (AP_3v3)".
  Chip rev v0.2.
- USB: the board's native USB-Serial/JTAG port (VID 303a PID 1001, `/dev/cu.usbmodem101` on the Mac).
  Flashing and console both use it.
- **No USB link from the ESP32 to the printer.** The printer would try to charge from the ESP's USB host port.

## Build (Docker, nothing installed on the Mac)

```sh
cd firmware
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5 idf.py build
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5 idf.py merge-bin -o m.bin
cp build/m.bin a80-airprint-stepN.bin       # single image, flash at 0x0
```

- After editing `sdkconfig.defaults`, **delete `sdkconfig`** so the defaults are re-applied.
- Image: `espressif/idf:v5.5` (11.7 GB). The managed component `espressif/mdns` (1.15.x) is fetched on the first build.
- The Mac's clangd shows lots of "file not found" errors for ESP-IDF headers. Ignore them; the real build is in Docker.

## Flash (web tool)

Chrome → https://espressif.github.io/esptool-js/ → Connect (baud 460800, "USB JTAG/serial debug unit")
→ address **0x0** → the merged `.bin` → Program → **Disconnect** → RESET.
To see logs: Console section → 115200 → Start → RESET.
If Connect fails: hold BOOT, tap RESET, release BOOT.

**Rules learned the hard way:**
- **Only one program can hold the serial port.** Never leave a background serial logger running on the Mac. It
  grabbed the port after "Disconnect" and corrupted a flash. The symptom was the ROM reporting
  `Invalid image block, can't boot` with ASCII garbage lengths. Recovery: Erase Flash, then reflash at 0x0.
- The S3's USB-Serial/JTAG disconnects on every chip reset, so boot loops are hard to capture from the Mac.
  Use the web console.

## Source layout (`main/`)

| File | Role |
|---|---|
| `config.h` | SSID (`A80-Print`, open), channel 6, printer name, hostname `a80printer`, port 80 (printing and web pages), UUID, head width 1664 |
| `main.c` | NVS, netif, SoftAP (WIFI_AUTH_OPEN), mDNS + TXT record, starts httpd and printer |
| `httpd.c` | Minimal HTTP/1.1 on lwIP sockets. 8 worker tasks with **PSRAM stacks** take connections from a queue. Idle keep-alive timeout is 10 s; 120 s while a request body is arriving. Handles `Expect: 100-continue` and **chunked** bodies. Reads only the IPP header into memory, then **streams** the document to `ipp_doc()`. |
| `ipp.c` | IPP encode/parse, printer attributes , job table (8). API: `ipp_header_len` / `ipp_begin` / `ipp_doc` / `ipp_finish`, plus `ipp_job_update` / `ipp_job_canceled` for the print task |
| `urfconv.c` | Byte-level streaming URF decoder → area-average resize to 1664 → levels clamp (≤7→0, ≥248→255) → Floyd-Steinberg → packed 1-bit page in PSRAM (~450 KB per Letter page). Host-tested on the Mac against captured jobs (see below). |
| `printer.c` | NimBLE central: scan for `A80_XXXX_LE`, MTU 247, discover all characteristics, discover each CCCD within its handle range, subscribe to every notify/indicate characteristic except FEC8. Credits come on FF03 (`01 nn`), replies on FF01. Print task: status `10 FF 40` before each page (hold + IPP state on error), stream the page with credits, reprint on pushed `ff 01`/`ff 02`. Idle: status + battery every 30 s for 5 min after connecting or printing, then silent so the printer can power off. |
| `weblog.c` | `weblog()` = ESP_LOGI with tag `a80` (plus an unused ring buffer). The status web page is `printer_status_html()` in `printer.c` |

## Findings from the ESP32 bring-up

- **iOS sends every IPP request with `Transfer-Encoding: chunked`.**
- **A photo printed from iOS can be over 4 MB of URF** (300 dpi 8-bit gray barely compresses). A 1-page text PDF
  was about 1.2 MB. Buffering whole documents failed: realloc doubling wanted an 8 MB block. **Stream-convert instead.**
- Memory after Wi-Fi + BLE + mDNS: about 148 KB internal RAM free and almost all 8 MB PSRAM free. Settings that matter:
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`,
  `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`, TCP windows 65534.
- Upload speed over the hotspot was about 1.2 MB in 13 s, while converting on the fly. Fine for now; it could be optimised.
- **NimBLE: incoming notifications are dispatched from the ATT *server* code** (`ble_att_svr.c`). With
  `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n` the GATT server is compiled out and **every notification is silently
  dropped**, even though discovery and CCCD writes succeed. That caused the "printer status -1 / not responding" hold.
  Fixed in sdkconfig.defaults (peripheral role and GATT server on). Not yet verified on hardware.
- A80 GATT layout seen from the ESP32: FF02 val 3, FF01 val 5, FF03 val 8 (CCCD 10). FF01's descriptor
  discovery returned ATT error 0x02. FFF1 val 42 / cccd 43, plus two 128-bit notify characteristics at 50/51 and 53/54.
- iOS joins the open hotspot (gets 192.168.4.2), sometimes leaves and rejoins once right after joining (reason 8).

## Host test of the converter (no hardware needed)

```sh
cd firmware && test/run.sh path/to/job.urf   # writes /tmp/<job>.pbm (1664-wide 1-bit pages)
```
To get a test file, capture the URF document from an iOS print job (the IPP Send-Document body).
