# AirPrint side: what the iPhone needs (verified 2026-11-10 with iOS, Chrome PDF and Photos)

Implemented in `firmware/main/ipp.c` and `firmware/main/urfconv.c`.
The ESP32-S3 firmware should reproduce this conversation exactly.

## Bonjour / mDNS advertisement

Service `_ipp._tcp`, subtypes `_universal` (this marks it as AirPrint) and `_print`, port 80. TXT record:

```
txtvers=1 qtotal=1 rp=ipp/print ty=aixiqee A80 product=(A80) note=Desk
pdl=image/urf Color=F Duplex=F Copies=F UUID=<uuid> URF=CP1,IS1,MT1,RS300,V1.4,W8
kind=document,roll PaperMax=legal-A4 priority=0 adminurl=http://<host>/
```

Plain IPP (no TLS) is accepted by iOS.

## IPP operations iOS uses (observed order)

1. `Get-Printer-Attributes`, repeatedly. Requested attributes include printer-state-reasons, media-source-supported,
   printer-input-tray, printer-mandatory-job-attributes, pages-per-minute-color, … Returning the full set every time is fine.
2. `Validate-Job` with document-format=image/urf.
3. **`Create-Job`** with job attributes media-col (size, margins), orientation-requested=3, print-color-mode=monochrome,
   print-quality (4 or 5).
4. **`Send-Document`** with last-document=true, document-format=image/urf, compression none, body = the whole URF file.
5. `Get-Job-Attributes` polling (job-state, job-state-reasons, job-impressions-completed,
   job-media-sheets-completed) and `Get-Printer-Attributes` (printer-state-reasons) polling every 1–5 s while printing.

HTTP: POST /ipp/print, `Content-Type: application/ipp`. Handle `Expect: 100-continue` and chunked bodies.
Reply with the same IPP version as the request. Reject request-id 0, a missing charset/natural-language
(as the first two attributes), or a missing printer-uri, with 0x0400; reject a version other than 1.x/2.x with 0x0503.

## Key printer attributes (full list in `printer_attributes()` in `firmware/main/ipp.c`)

- document-format-supported = image/urf (only). urf-supported = CP1,IS1,MT1,**RS300**,V1.4,**W8** (8-bit gray).
- media: na_letter_8.5x11in (default), iso_a4_210x297mm. **The default media-col has 5 mm margins and the database
  also has 0-margin (borderless) variants.** If only 0 margins are offered, iOS *fills and crops* photos.
- print-scaling-supported auto,auto-fit,fill,fit,none; color-supported=false; print-color-mode monochrome.
- printer-state 3/4/5 + printer-state-reasons (`media-empty-error`, `door-open-error`, `offline-report`) drive iOS status.

## What iOS sends (URF)

`UNIRAST\0` + page count. Per page: 8 bpp gray, 300 dpi, **2550×3300 for Letter** (iOS renders the full page,
margins included). Row-based PackBits-style compression (see `urfconv.c`). A 1-page text PDF was 1.3 MB; a logo photo 118 KB.

## Conversion pipeline (per page)

URF rows → resize to 1664 px wide (Lanczos) → gray → **levels clamp (≤7→0, ≥248→255)** → Floyd–Steinberg → A80
job (darkness=high, roll, feed_mm=4, 256-row bands) → BLE credit sender (status check first).

The levels clamp is required. iOS photo backgrounds come through as 254, not 255, and dithering turns that into faint
diagonal dot lines.

## Error path (verified 2026-11-10 21:39–21:43)

- Before each page the worker queries `10 FF 40`. If it isn't `00`, it sends nothing, holds the job, retries every ~10 s and reports:
  printer-state=5 (stopped), printer-state-reasons=`door-open-error` (cover flag 0x02, checked first because a
  cover-open printer also reads no-paper) or `media-empty-error` (0x04), and job-state=6 (processing-stopped) with
  job-state-reasons=`printer-stopped`.
- iOS showed an "out of paper" alert within seconds. Once the cover was closed with paper, the held job printed
  automatically about 3 s later and the iPhone marked it done.
- Retest with cover-first mapping and job-state=6: iOS showed **"The printer door is open"**. The alert is a ~2 s banner
  either way (iOS behaviour; the current status lives in Print Center). There's nothing more to do on the printer side.

## Gotchas found

- macOS's `ippeveprinter` (CUPS 2.3.4) rejects the modern iOS Create-Job ("Unexpected document data following
  request"), so it can't be used as the server. It was still useful for capturing a reference attribute set.
- The iPhone caches printer capabilities. After changing attributes, re-select the printer on the phone.

## Seen on the ESP32 (2026-11-10)

- iOS sends **every** IPP request with `Transfer-Encoding: chunked`.
- URF upload sizes: 1-page text PDF ~1.2 MB; a **photo over 4 MB** (8-bit gray at 300 dpi barely compresses).
  Small devices must **stream-convert** the document rather than buffer it (see firmware/main/urfconv.c).
- iOS polls Get-Printer-Attributes and Get-Job-Attributes every ~1–3 s on separate keep-alive connections while uploading
  on another. The server must serve them concurrently.

## Naming the paper types (`printer-strings-uri`)

`printer-strings-uri` (with `printer-strings-languages-supported` = `en`, `en-us`) points to a `text/strings` message
catalog (PWG 5100.13 section 9.1) served by the printer at `http://<host>/strings/en.strings`. Keys are
`attribute.keyword`. The firmware advertises the two paper types with **its own keywords** and names them there:

```
"media-type.a80-continuous-perforated-letter" = "Continuous Perforated Letter";
"media-type.a80-paper-roll" = "Paper Roll";
```

**Confirmed on iOS and macOS (2026-11-14):** Media & Quality shows "Continuous Perforated Letter" and "Paper Roll".
Findings that led here:

- With the standard keywords (`continuous-short`, `stationery`) the iPhone fetched the catalog but kept its built-in
  names ("Continuous Paper - Short Edge", "Plain Paper"); the Mac showed the same. Built-in names win for keywords
  the OS knows.
- Free-text (`name`) values are no option: Apple's open-source CUPS looks a value up in the printer's catalog first
  (`media-type.<value>`), then in its built-in table, and shows an unknown name as "Other Media".
- Keywords the OS doesn't know are named from the catalog, which is what the firmware now relies on.

`A80_MEDIA_TYPE_NAMES` in `config.h`: 0 = standard keywords, 1 = free-text names, 2 = our keywords + catalog.
Jobs naming any of these spellings are accepted.

## One port: 80

Printing (IPP) and the web pages (status page, `/wifi`, the paper-name catalog) share **port 80**, so
`http://192.168.4.1/` and `http://a80printer.local/` work in a browser. Bonjour advertises port 80 in the service
record, and `printer-uri-supported` is `ipp://a80printer.local:80/ipp/print`. Earlier firmware used 8631 for both;
nothing in the AirPrint conversation requires a particular port, because Bonjour carries it.

## Android: PWG raster (`image/pwg-raster`)

Android's built-in print service (the Mopria-based "Default Print Service") finds the printer through the same
`_ipp._tcp` Bonjour record but never sends `image/urf`: it sends PDF, PCLm or PWG raster, and a printer that offers none
of those isn't usable. So the printer also accepts **PWG raster** (PWG 5102.4), advertised as
`document-format-supported = image/urf, image/pwg-raster`, `pwg-raster-document-resolution-supported = 300dpi`,
`pwg-raster-document-sheet-back = normal`, `pwg-raster-document-type-supported = sgray_8` (and `pdl` in the TXT record).
Research and the plan are in issue #27. **Tested on an Android tablet (Chrome, Letter, portrait): prints correctly over home Wi-Fi and over the printer's own no-internet hotspot at reduced power.**
Android labels paper sizes itself from a fixed list in its print service ("Letter", "ISO A4"); it ignores `printer-strings-uri`,
so the custom paper names the iPhone and Mac show can't appear there.

The reader (`firmware/main/urfconv.c`) sniffs the first bytes: `UNIRAST` = URF, `RaS2` = PWG raster. The row coding is
the same in both (a repeat count, then PackBits-like runs); only the headers differ.

| | URF | PWG raster |
|---|---|---|
| File header | 12 bytes: `UNIRAST\0`, page count | 4 bytes: `RaS2`; no page count (the stream ends after the last page) |
| Page header | 32 bytes: bits per pixel at 0, width 12, height 16, dpi 20 | 1796 bytes, big-endian: `HWResolution` 276, `Width` 372, `Height` 376, `BitsPerColor` 384, `BitsPerPixel` 388, `BytesPerLine` 392, `ColorOrder` 396 (0), `ColorSpace` 400 (18 = sgray, 19 = srgb) |

Accepted PWG types: `sgray_8` (8-bit gray, what Android sends a monochrome printer) and `srgb_8`. Others are refused
with `document-format-error`. A PWG stream is complete when it ends between pages.

For a job from a PWG-raster client, only our roll name (`a80-paper-roll`) selects the roll. Android's default paper type
words (`auto`, `stationery`) print on the continuous paper. An iPhone job still treats the old standard keyword
`stationery` as the roll, for phones that remember the old names.

Test files: `firmware/test/mkpwg.py` writes PWG raster from the spec; its output was decoded with CUPS's own
`cupsRasterReadHeader2`/`cupsRasterReadPixels` (40 random images, identical pixels) before the fixture was committed.
CI runs `firmware/test/raster_host_test.c` against `firmware/test/fixtures/checker.pwg`.
