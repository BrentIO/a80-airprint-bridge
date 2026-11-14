# A80 A4 Thermal Printer — Device & Protocol Notes

Reverse-engineered from the vendor's own drivers (macOS pkg v1.2.7, Linux .run v1.2.3).
Nothing was installed; both were unpacked into a scratch directory only.

## Device identity

| Field | Value |
|---|---|
| Label brand | aixiqee (FCC ID 2A6FW-A80) |
| USB vendor / product | `0x5958` ("YXWL Mi") / `0x0800` ("Printer") |
| USB class | 7/1/2: standard USB Printer Class, bidirectional |
| IEEE-1284 ID | `MFG:YXWL;MDL:A80;CMD:XPP,XL;CLS:PRINTER;DES:A80;` |
| CUPS URI | `usb://YXWL/A80?serial=A8024xxxxxx` |
| Maker | Xiamen Print Future Technology Co., Ltd. (FCC grantee 2A6FW); support via FlashLabel. Details: `DEVICE.md` |
| Bluetooth chip | Barrot **BR8051A01** (dual-mode BT, own MCU + flash), firmware `_231113_r1389` (`10 FF 30 10`) |
| Main controller | **Unconfirmed.** Candidate Artery AT32F403AR (seen in sibling model C80). Runs firmware **V1.08**, boot loader **1.01** |
| Model / serial | `A80` (`10 FF 20 F0`) / `A8024xxxxxx` (`10 FF 20 F2`) |
| Bluetooth names | Classic `A80_XXXX` (MAC 60:6E:41:XX:XX:XX); BLE `A80_XXXX_LE` (MAC C0:6E:41:XX:XX:XX) |
| Interfaces | USB-C (USB Printer Class + charging, 5 V / 2 A), Bluetooth classic (SPP) and BLE. Same byte stream on all |
| Resolution | 203 dpi (the "A80H" variant is 300 dpi) |
| Data format | 1-bit monochrome raster only (no PDF/PS/PCL in the printer) |

## Vendor driver structure

- A PPD plus a CUPS filter `rastertolabel`, which is a fork of the stock CUPS
  `rastertolabel` sample (Zebra/Dymo code is still present). The A80 uses `cupsModelNumber 20`.
- The macOS pkg also installs a root LaunchDaemon (`com.launch.A80`, KeepAlive) that loops
  `system_profiler SPUSBDataType` and `lpadmin` to auto-create a queue. It's harmless but
  unnecessary.
- The Linux filter is **x86-64 only**, so it won't run on a Raspberry Pi or other ARM box.
  It isn't stripped and still has debug info, which is how the protocol below was recovered.

## Page sizes (from PPD, in points)

A4 = 572×826 pt (≈1612 dots wide at 203 dpi), Letter = 589×790, max width 600 pt, max length 8000 pt.
Physical head width is unverified. It may be 1728 dots (216 mm); test this.

## Job byte stream (one page)

```
00 × 128                      # leading NULs (wake / flush)
10 FF F1 03                   # init / start job
1F 90 01 D2                   # only if page width == 572pt (A4)
1F 90 01 D8                   # only if page width == 589pt (Letter)
1F 80 01 <m>                  # media type: 10=Roll (continuous), 20=Label (gap),
                              #             30=Fold (fanfold, mark), 40=Tattoo (continuous)
10 FF 10 00 <d>               # darkness: 00=Low, 01=Medium, 02=High (vendor PPD names; PPD default High)
                              # ESP32 (step17): iPhone offers only Normal and Best. Best=02, Normal (default) = automatic:
                              # 01 for pages > 50% black, else 02
1F 11 51                      # only for non-continuous media (Label / Fold)
1D 76 30 00 xL xH yL yH       # GS v 0: raster image, x = bytes/row, y = rows (≤ 65535)
<x*y bytes>                   # 1bpp, MSB = leftmost pixel, 1 = black
```

End of page:
```
# non-continuous (Label/Fold):
1D 0C                         # GS FF: feed to next gap/mark
# continuous (Roll/Tattoo):
1B 4A nn ...                  # ESC J feed; total = 8*FeedOffset_mm + 128 dots (Tattoo: +240),
                              # sent in chunks of 255
# always:
10 FF FE 45                   # end job
```

## Image processing done host-side by the vendor filter

8-bit gray in. Then brightness and contrast (0–255, default 128), then one of three modes:
threshold (default 128), "Effect1" ordered/pattern dither (the default), or "Effect2"
random/error diffusion. Last step: a pixel ≤ 200 becomes a black dot.

## Verified on hardware (2026-11-10)

- **Print head = 1664 dots (208 bytes/row, ~208 mm).** Rows are centered on the head:
  a narrower image gets equal white margins, and a wider one (1728) is clipped evenly on both sides.
- The printer answers `4F 4B` ("OK") on the bulk-IN endpoint / BLE notify after the end-of-job command.
- **Splitting a page into several `GS v 0` bands leaves no visible seams.** This means pages can be
  streamed in strips.
- The printer does no halftoning: dither on the host (Floyd–Steinberg looks good).

## Bluetooth LE

- Advertises as `A80_XXXX_LE` (manufacturer data contains MAC 60:6E:41:XX:XX:XX). MTU 247, so 244-byte writes.
- Several UART-style services are present (FF00, FF10, 18F0, FEE7, FF80, FFF0, ISSC 49535343-…).
  Tested working: service `18F0`, write `2AF1`, notify `2AF0`.
- It's the same byte stream as USB.
- **GATT layout as seen by the ESP32** (handle = value handle; props: 04 write-no-response, 08 write, 10 notify, 20 indicate):

  | Characteristic | Handle / CCCD | Props | Use |
  |---|---|---|---|
  | `FF02` (service FF00) | 3 / — | write | **Job data and commands** (the ESP32 writes here) |
  | `FF01` (service FF00) | 5 / 6 | 10 | **Printer replies** (`4F 4B`, status, query answers) and pushed events `ff xx` |
  | `FF03` (service FF00) | 8 / 10 | 10 | **Flow control**: `01 nn` = nn more packets allowed, `02 f4 00` = packet size 244 |
  | `FF11`, `FF12` (service FF10) | 13/14, 16/17 | 14 | unused |
  | `2AF0` (service 18F0) | 22 / 23 | 10 | replies, alternate channel (pairs with write `2AF1`) |
  | `FEC8` (service FEE7) | 28 / 29 | 20 | vendor/WeChat-style ID service; not subscribed |
  | `FF81` (service FF80) | 36 / 37 | 10 | unused |
  | `FFF1` (service FFF0) | 42 / 43 | 20 | unused |
  | ISSC `49535343-1e4d-…` | 50 / 51 | 10 | pushed events (`ff 01`, `ff 02`, `aa`) also appear here |
  | ISSC 128-bit (second) | 53 / 54 | 18 | unused |

  Readable characteristics (FEC9 #30, ISSC #45) return nothing. No characteristic changes with the charger.
- Advertising: `A80_XXXX_LE`, MTU 247 (244-byte payload). On connect FF03 grants 7 packets.
- **Pacing matters:**
  - Acknowledged writes on every packet run at 7.3 KB/s. That is slower than the print speed,
    so the motor stops and starts, giving light, blotchy, noisy prints.
  - All writes unacknowledged: the buffer overflows and only the first few rows print.
  - Unacknowledged writes with every 8th one acknowledged: 19 KB/s, smooth, USB-equal quality
    (a full A4 page takes about 26 s).

## Bluetooth LE flow control (verified 2026-11-10). Use this

- Write the job to **`FF02`** (service `FF00`) without response, **one credit per 244-B packet**.
- On subscribe, `FF03` sends `01 07` (7 credits) and `02 f4 00` (payload 244). After that it sends `01 01` for every
  packet forwarded. In testing: 335 grants for 335 packets, no stalls, ~20 KB/s, USB-quality print at darkness=high.
- Printer replies on this service arrive on **`FF01`**: `4F 4B` and status bytes.
- **Don't** use the fixed "ack every 8th packet" pacing on `2AF1`. It happens to work at medium but **overflows at
  high darkness** (the printer prints more slowly). Bytes are silently dropped, and the printer is left mid-`GS v 0`
  waiting for rows. It then swallows every later byte, including queries, as image data. It looks hung.
- **Recovery without a power cycle:** send zero bytes (blank rows) until the image completes, e.g. 208×60 B,
  then `10 FF 40` answers again.
- **`OK` (`4F 4B`) acknowledges the job START** (`10 FF F1 03`, about 500 B in), not completion. Check completion
  with a status query.

## Idle behaviour

**Current rules (confirmed 2026-11-12):**
- **Any command over BLE (e.g. the `10 FF 40` status or `10 FF 50 F1` battery query) restarts the printer's
  auto power-off timer.** An open BLE connection with no traffic does not. Polling every 30 s kept it on for 108+ min.
- **Auto power-off is set to 5 min** (`10 FF 12 00 05`, read back 5; factory 20). The setter takes minutes: 60 and 5 both
  read back correctly. The ESP32 polls for 5 min after connecting or printing, then stops, so the printer turns
  itself off about 10 min after the last activity. Power-off drops the BLE link (`reason 520`).
- A switched-off printer can't be woken over Bluetooth. Plugging in the charger doesn't turn it on (the red LED is the
  charge light). Jobs wait with "Printer not connected" on the iPhone until someone presses the power button.
- The "asleep but answering" state below was seen with the old 20-min setting while polling. It hasn't recurred.

**History:**

- After about 5 min with no BLE connection, the printer **stopped advertising BLE** while still powered on (a short
  button press fed paper and brought the advertising back). This is separate from the 20-min auto power-off (`10 FF 13`).
- **Keep-alive verified (2026-11-10 21:04–21:25):** staying connected and sending `10 FF 40` (via FF02) every 60 s kept
  the printer connected and answering `00` for 21+ minutes, past the 5-min BLE quiet point and the 20-min
  auto-off. (Superseded: polling is what kept it on; see the current rules above.)
- **CORRECTION (2026-11-11): answering `00` does NOT mean the printer is awake.** With the charger connected and the
  ESP32 polling `10 FF 40` + battery every 30 s, the print engine went to sleep (presumably the 20-min auto power-off)
  while the BLE side stayed connected and kept answering status `00`, battery, density, and even `4F 4B` to job start.
  It silently discarded two whole jobs: each 446 KB page was accepted in 10.7 s (~41 KB/s, twice real print speed)
  and status read `00` right after, instead of `01` (printing). A power-button off/on fixed it; that **did** drop the BLE
  link (`reason 520`, supervision timeout) and the ESP32 reconnected within 1 s. Not proven whether the engine had
  auto-powered-off (20 min, status polling apparently doesn't reset it) or was stuck.
- On BLE reconnect after a printer power-up, FF01 carried `AT+LBDADDR?\r\n`: the BLE bridge chip's own boot-time AT
  command to the printer MCU, leaking to the central. Harmless.
- **Battery % is voltage-based and sags under load:** after two 71%-black Letter pages on the charger it read 68% →
  28%, then recovered to 57% within 5 min. Don't treat a drop right after printing as "charger removed".
  Detection (firmware step11): page > 150 KB taken at > 30 KB/s **and** status `00` right after = not printed.
  Mitigation: auto power-off raised to 60 min with `10 FF 12 00 3C` (user-approved setter).

## Paper path

- The tear bar sits about **15–16 mm past the print head**. Measured with reliable (credit) flow control:
  20 mm of feed after the content cleared the bar by about 7 mm, and 30 mm cleared it by about 15 mm.
  Default: `feed_mm=4` (4 mm + 16 mm fixed = 20 mm).
  An earlier reading of "about 27 mm" came from a job sent with the unreliable pacing; ignore it.
- Measured: a Letter page (shrunk to fit 1664 dots, `feed_mm=4`) tears off about **4 mm longer than 11"** (~283 mm).
  Accepted as-is (2026-11-10); no "true size" mode for now.
- On a roll the blank above a page equals the previous page's trailing feed. A reverse feed might
  trim it (untested).

## Status reporting (verified over BLE, 2026-11-10)

- The printer does **not** answer ESC/POS `DLE EOT 1–4`, or Phomemo-style `1F 11 xx` queries (tried 07/08/0E/11/12/13).
- It **pushes** state changes on its own, on notify characteristic
  `49535343-1e4d-4bd9-ba61-23c647249616` (ISSC service). Verified one step at a time:

  | Event | Bytes |
  |---|---|
  | Cover opened | `ff 02` |
  | Cover closed with no paper | `ff 01` |
  | Cover closed after paper was reloaded (clears paper-out) | `aa` |
  | Cover closed, paper was already in | **nothing** |

- Nothing is sent on connect, so the current state can't be learned except from transitions.
- **A job sent while the cover is open is silently discarded.** The printer still replies `OK`, never
  reports an error, and doesn't print the job when the cover closes. So `OK` means "job received", not "printed".

- Characteristic `FF03` sends `01 07` then `02 f4 00` on connect. This is probably the BLE module's
  flow control: `02` = MTU payload 244, `01` = credits (7). Not yet used.
- After the end-of-job command the printer answers `4F 4B` ("OK") on `2AF0` (BLE) and on bulk IN (USB).
### Queries (`10 FF xx`): reply on `2AF0` (BLE). Verified 2026-11-10

| Query | Reply | Notes |
|---|---|---|
| `10 FF 40` status | 1 byte of flags | `00` ready · `01` **printing** (seen right after a big job, then cleared) · `02` cover open · `04` no paper. Cover open reads `06`, because the paper sensor also loses the paper. |
| `10 FF 50 F1` battery | `00 nn` | nn = percent (0x45 = 69). Varies ±1 between reads; it wasn't charging. |
| `10 FF 20 F0` model | ASCII `A80` | |
| `10 FF 20 F1` firmware | ASCII `V1.08` | |
| `10 FF 20 F2` serial | ASCII `A8024xxxxxx` | Same as the USB serial. |

Verified sequence: closed with paper → `00`; opened → `06`; closed with no paper → `04`;
reloaded and closed → `00`. The status query catches the "closed with paper" transition, which the pushed
events don't. **Driver rule: query `10 FF 40` before every job and only send when it reads `00`.**
Pushed events are still useful for noticing changes while idle.

- Still unknown: whether USB reports these events and queries the same way, and what happens when
  paper runs out mid-job.

## Open questions / TODO

- [x] **Charging state: no direct indicator exists** (concluded 2026-11-10 20:53). Plugged and unplugged, these are
      identical apart from the battery %: `10 FF 70`, `10 FF 11`, `10 FF 13`, `10 FF 40`, and the `10 FF 50 F1` first byte.
      The % reads high while charging (~90–100) and stays elevated just after unplugging (92–94; it read 60 before
      charging). Only a heuristic is possible. Details: (2026-11-10 20:30):
      - `10 FF 50 F1` stays `00 nn` either way, and the status byte stays `00`.
      - Nothing is pushed on plug or unplug.
      - **Re-checked 2026-11-11 (read-only GATT probe):** subscribed to every notify/indicate characteristic
        (except FEC8) and read every readable one (only FEC9#30 and 49535343-…#45, both empty) every 20 s through
        plug and unplug: nothing pushed, nothing changed. Battery read 13% unplugged, **34% right after plugging in**
        (+21). The ESP32 firmware (step8) infers charging from a jump of 8 or more points.
      - Indirect sign: the battery % reads from voltage. Unplugged it read a steady 60% (`3c`); plugged in it
        jumped to 74–80% (`4a`/`50`) and wobbled between reads. The button LED blinks green while charging.
      - Probed 2026-11-10 20:33, charger connected, with a `10 FF 40` canary after each:
        `10 FF 50 F0, F2–F8` gave no reply and the canary was fine.
      - **`10 FF 20 F3` is NOT a query.** It gave no reply, and the following status query got `4F 4B` ("OK")
        instead of `00`. It looks like a setter that took the canary's bytes (`10 …`) as its value. Effect
        unknown; the printer still answers normally afterwards. Watch for changed behaviour (auto-off time,
        BT name, defaults). Probing stopped there; `10 FF 20 F4+` are untested.
- [x] **Paper jam** (2026-11-10 21:02, real jam during a one-page test at high): **the printer has no jam detection.**
      It pushed nothing, credits kept flowing (the whole 448 KB page was accepted in 28 s), and it kept printing
      through the jam. The driver can't detect a jam; only a person watching can.
- [x] **Paper out mid-job** (tested 2026-11-10 20:58, two Letter pages back to back, credit sender, high):
      - The printer pushed `ff 01` on **FF01** (the channel in use) when the sensor tripped. Transfer was at byte
        210,572 of page 2. The paper physically printed to row ~900 (~187 KB of image), so the sensor fires a little
        before the paper actually ends.
      - **Credits kept flowing:** the remaining ~238 KB was accepted in 9 s and **silently discarded**.
      - Status `04` while out; `ff 02`/`06` on open; `00` after reload and close. **No `aa` this time** (that one was
        seen on the ISSC channel). After reload **nothing printed**: the rest of the page is lost.
      - **Driver rule:** watch for `ff 01` (or poll `10 FF 40` ≠ 00) during a job, stop sending, hold the job,
        wait for status `00`, then **reprint the whole page**.
- [ ] **Media types:** support both
      - **Rolls (continuous):** `1F 80 01 10`; page length = the content (or a fixed A4/Letter length),
        then feed past the tear bar (≥36 mm).
      - **Full pages (A4/Letter fanfold or cut sheets):** `1F 80 01 30` (Fold) plus `1F 11 51`, ending with
        `1D 0C` (feed to the next page mark/gap), and `1F 90 01 D2/D8` for the A4/Letter paper size.
        Not tested yet: how the printer finds the page boundary, sheet length limits, behaviour with a
        single cut sheet.
- [x] **Printer hang (2026-11-10 20:05):** after a strip at darkness=high with large solid-black
      areas, on battery, the printer stopped answering every query (the BLE module stayed up) and the
      trailing feed fell short. Power cycle needed. Feed commands were ruled out (`1B 4A FF`,
      `1B 4A FF 1B 4A 21 10 FF FE 45` are fine). Reproduced at 20:15 with a small, mostly-white job
      (test A), so power sag is ruled out. Both hung jobs used darkness=high (`10 FF 10 00 02`) **and** a
      `10 FF 40` status query just before the job; every job that worked used medium with no query.
      **SOLVED:** BLE buffer overflow from fixed pacing at high darkness (see the flow control section).

## Product family: commands from community reverse engineering (2026-11-10)

The A80 is in the same white-label `10 FF` family as the Fichero/AiYin D11s and the Lidl A2Y
(Lujiang). It is *not* the Phomemo M08F `1F 11` family. Sources:
[fichero PROTOCOL.md](https://github.com/0xMH/fichero-printer/blob/main/docs/PROTOCOL.md),
[lidlprint](https://github.com/mgurzixo/lidlprint),
[thermoprint RE notes](https://github.com/tomLadder/thermoprint/blob/main/REVERSE_ENGINEERING.md),
[M08F reference](https://github.com/PassionateBytes/phomemo-driver/blob/main/docs/m08f-protocol-reference.md).

| Bytes | Documented meaning | A80 status |
|---|---|---|
| `10 FF 20 EF` | boot version (ASCII) | ✅ `1.01` |
| `10 FF 20 F3` | BT MAC (thermoprint only) | no reply; following query got `OK` |
| `10 FF 11` | get density (3 bytes) | ✅ `02 0f 00`: byte 0 = last density set (02 = high); bytes 1–2 unknown |
| `10 FF 13` | get auto power-off (2 B big-endian, minutes or seconds; sources disagree) | ✅ `00 14` = 20 (minutes, presumably the default) |
| `10 FF 12 HH LL` | **set** auto power-off | setter, don't probe |
| `10 FF 70` | all info: `NAME\|MAC\|MAC\|FW\|SERIAL\|BATT` | ✅ `A80_XXXX\|60:6E:41:XX:XX:XX\|C0:6E:41:XX:XX:XX\|V1.08\|A8024xxxxxx\|94` (classic MAC, LE MAC) |
| `10 FF 30 10` / `30 11` | BT module version / BT name | ✅ `_231113_r1389` / `A80_XXXX` |
| `10 FF 84 nn` | **set** paper type (0 gap, 1 mark, 2 continuous), Fichero | setter |
| `10 FF C0 nn` | **set** speed | setter |
| `10 0C` | form feed, replies OK | untested |
| `10 FF 40` bits (Fichero) | 01 printing, 02 cover, 04 paper, 08 low batt, 10/40 overheat, 20 charging | 02/04 confirmed; **charging bit NOT set on A80** |
| pushed `FF xx` (thermoprint) | 01 paper out, 02 cover open, 03 overheat, 04 low battery, 05 cover closed | 01/02 confirmed |

**NEVER SEND:** `10 FF 04` (factory reset), `10 FF E0 AA AA` / `10 FF FF ..` (firmware update),
`10 FF A0 04/06` (OTA), `1F 50 BE` (factory reset), `1F A0 BE 66 88` (bootloader).

**Flow-control caveat:** the M08F author found the `FF03` `01 01` messages are only bridge-chip echoes, not real
credits. Their printer overflowed about two-thirds of the way into an A4 page with unacknowledged writes (~100 KB
buffer). Our credit sender has only been tested on short jobs. **Tested 2026-11-10 20:41:** a full Letter page (448 KB, 1664×2154) at
darkness=high via the credit sender printed complete and smooth with no overflow. It took 29.6 s (~15 KB/s, slower than short
jobs, which is consistent with real back-pressure). Status read `01` (printing) right after sending, then `00`.

## Battery

- `10 FF 50 F1` → `00 nn`, nn = percent. It follows **voltage**, not charge:
  - **Noise:** idle on battery it wobbles 2–3 points between reads (26/28/29/26…).
  - **Sag:** heavy printing drops it hard, then it recovers over ~5 min (72 → 49 → 57; 34 → 14 → 29).
  - **Charger:** plugging in jumps it (13 → 34) and it stays inflated while charging; it climbs steadily overnight.
- **No charging indicator exists** anywhere (status bit 20 is never set; nothing is pushed on plug/unplug).
  ESP32 guess (step22+): the lowest of the last 4 readings (2 min) rises 8+ above the low = charger connected;
  the highest of the last 4 falls 8+ = unplugged; 100% three times in a row = on the charger. No guesses for
  5 min after printing.
- **Low battery:** the ESP32 reports only the printer's own flag, status bit `08` (step25+). Not yet seen set,
  even at 14–20%.
- A low battery makes dark pages slower and noisier.

## Settings stored in the printer

| Setting | Read | Write | Current value |
|---|---|---|---|
| Auto power-off (minutes) | `10 FF 13` → `HH LL` | `10 FF 12 HH LL` → `4F 4B` | **5** (set by the ESP32 on connect; factory 20) |
| Darkness | `10 FF 11` → `dd 0f 00` (dd = last used) | per job: `10 FF 10 00 dd` (00/01/02) | per job: Normal = auto (01 if >50% black, else 02), Best = 02 |
| Paper type | — | `10 FF 84 nn` (untested setter) | not used; per job `1F 80 01 10` (roll) |
| Speed | — | `10 FF C0 nn` (untested setter) | not used |

## Memory and buffers

- **Printer receive buffer:** unknown size. Flow control on FF03 grants 7 packets at connect, then one per packet
  forwarded; a page streams at 10–20 KB/s, set by the printer. Without credits it overflows (the M08F author saw
  ~100 KB). A job sent while asleep or with the cover open is accepted and silently discarded.
- **Page size in the printer's terms:** 1664 dots × 208 bytes per row; a Letter page is 2142–2153 rows ≈ 448 KB of
  1-bit raster, sent as `GS v 0` bands of 256 rows (53 KB each).
- **Printer chips:** Barrot BR8051A01 (own flash for the Bluetooth firmware). Main controller unconfirmed; if it is the
  AT32F403AR seen in the C80, it has 256 KB–1 MB flash and up to 224 KB RAM, a boot loader at the start of flash,
  and is normally read-protected. No firmware image is published (see `DEVICE.md`).
- **ESP32-S3 bridge:** 8 MB flash, 8 MB PSRAM. After Wi-Fi + BLE + mDNS ~140–148 KB internal RAM free. Each converted
  page (~450 KB) and the HTTP workers' stacks live in PSRAM. Up to 8 IPP jobs in the job table.

## Fanfold (continuous 8.5×11") paper

- **No mark sensor** (no light under the paper), so pages are advanced by an exact count: 2142 printed rows
  (`A80_FANFOLD_ROWS`) + a 120-dot feed (15 mm, tear bar) = one 279.4 mm sheet. Light pages advance ~0.7 mm per page
  more than dark ones; drift over 6 pages ≈ 4 mm before the last trim (FIX 32 → 27).
- Fold mode (`1F 80 01 30` + `1D 0C`) stops inconsistently: don't use it. Use roll mode with the exact step.
- **No reverse feed:** `ESC e`, `ESC j`, `ESC K` do nothing, and `ESC j`/`ESC K` swallow the following bytes.
- **Loading:** insert the paper; the printer pulls it to ~90–92 mm past the tear bar. Open the cover, slide the
  paper back so the perforation sits at the tear bar, close (closing doesn't feed).
- **Paper sensor ~7 mm before the tear bar:** the last sheet of a stack prints fully, but the final feed stops early
  and the printer reports out of paper (`ff 01`, ~1–2 s after the last byte). The ESP32 counts that page as printed.

## What iOS shows from the bridge (verified 2026-11-13, step29)

- **Known Printers list:** `printer-location` / Bonjour TXT `note` appears under the printer name. The ESP32 uses it as a status line.
- **Printer Info → Supply Levels:** shown only when the supply looks like a normal cartridge: `marker-types` `toner`,
  `marker-colors` `#000000` (type `other` / colour `none` gave "Information Not Available"). Used for the battery %.
- **Printer Info → Show Printer Web Page:** opens `printer-more-info` (`http://<address>/`; the status page is on port 80 and on 8631).
- **Print sheet:** shows nothing for `connecting-to-device`; errors (`printer-state` stopped) flash an alert.
