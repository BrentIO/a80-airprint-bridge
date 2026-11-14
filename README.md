# a80-airprint-bridge

ESP32-S3 AirPrint bridge for the **A80** portable A4 thermal printer: print from an iPhone over Wi-Fi, sent on to
the printer by Bluetooth LE. No vendor app, no driver, no computer.

```
iPhone ──Wi-Fi (AirPrint / IPP)──▶ ESP32-S3 ──Bluetooth LE──▶ A80 thermal printer
          its own hotspot "A80-Print"     converts each page to 1-bit
```

The A80 is sold under several brands (aixiqee, IOOIOO, FlashLabel, …). It's made by Xiamen Print Future Technology
(FCC ID 2A6FW-A80) and prints 8.5" / A4 wide at 203 dpi on thermal paper, from a roll or fanfold stack.
It's a printer similar to [this one on Amazon](https://a.co/d/09d2KUnP).

## What works

- The iPhone finds **A80 Thermal** on the ESP32's open hotspot and prints from any app.
- **Android** prints too (tested: Chrome on an Android tablet, over home Wi-Fi). Print → **All printers** or the printer list shows **A80 Thermal**.
  Android lists the sizes as plain **Letter** and **ISO A4** and warns that the print passes through other services; both are
  Android's own wording and can't be changed from the printer.
- **Paper:** **Continuous Perforated Letter** (fanfold; pages land on the perforations) is the default, and
  **Paper Roll** is the other choice, both under Media & Quality. A job that doesn't name a paper type prints as
  continuous perforated Letter.
- **Quality:** Normal = automatic darkness (lighter for very dark pages, which print more cleanly), Best = darkest.
- **Status on the iPhone:** a line under the printer's name ("Ready - battery 80%", "Off - press power button",
  "Out of paper"; with the printer off it adds how long ago it was last connected, e.g. "Off - press power button
  (last connected about 1h 25m ago)"), the battery under Printer Info → Supply Levels, and a status page under "Show Printer Web Page".
- **Printer off:** a job waits on the ESP32 and prints as soon as the printer is switched on (up to 30 min).
- **Errors:** if the cover is open or the paper is out when a page is due, the job waits and resumes once it's
  fixed; a fanfold stack running out on the last sheet counts as printed. (Errors in the middle of a page are not
  fully tested yet.)
- **Battery friendly:** the ESP32 stops talking to the printer 5 minutes after the last job, and sets the printer's
  auto power-off to 5 minutes, so it switches itself off about 10 minutes after printing.

## Hardware

- **ESP32-S3 with 8 MB PSRAM** (developed on an ESP32-S3-WROOM-1 N8R8: 8 MB flash, 8 MB octal PSRAM). The PSRAM
  holds the converted pages (~450 KB each).
- An **A80** printer (BLE name `A80_<id>_LE`). The 300 dpi "A80H" variant is untested.
- Power the ESP32 from any USB supply. Don't connect it to the printer's USB port.

## Build and flash

Build with Docker (nothing to install besides Docker):

```sh
cd firmware
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5 idf.py build
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5 idf.py merge-bin -o merged.bin
```

Flash `firmware/build/merged.bin` at address **0x0**, for example with the web flasher
(https://espressif.github.io/esptool-js/, Chrome) or `esptool.py write_flash 0x0 merged.bin`.

Every pull request is also built in GitHub Actions: download the images from the run's **Artifacts**, no local build
needed. There are three (the third is only for the one-time move below):

| Image | Flash at | Use it for |
|---|---|---|
| `a80-airprint-<id>.bin` (merged) | **0x0** | The first flash of a board, or after the bootloader/partition table changed. **Erases the saved home Wi-Fi.** |
| `a80-airprint-<id>-app.bin` | **0x10000**, or the web page `/update` | Updating a board that already runs this firmware. **Keeps the saved home Wi-Fi** and the paper setting. |
| `a80-airprint-<id>-partition-table.bin` | **0x8000** | Only once, to move a board with older firmware to the two-slot layout without erasing the saved Wi-Fi. |

The app address is **0x10000** (a 1 and four zeros), not 0x1000: flashing the app at 0x1000 overwrites the bootloader
and the board won't boot ("Invalid image block, can't boot"). If that happens, flash the merged image at 0x0 to
recover.

**Updating from the printer's own web page (no USB, no computer):** open `http://a80printer.local/update` (or
`http://192.168.4.1/update` on the hotspot; the status page links to it as "Update the firmware"), pick the `-app.bin`
file from the release or the CI run, tap **Update**, and wait. It takes about a minute; the printer restarts by itself
and **keeps the saved Wi-Fi**. It refuses while a print job is waiting or printing. A new version that fails to start
falls back to the old one, because it is only accepted after it has run for 30 s.

There is **no password** on it. Instead the hotspot's range is deliberately short (about 8 dBm, set by
`A80_AP_TX_POWER_DBM` in `config.h`), so you have to be close to the printer to join it. Be aware that this isn't
authentication: anyone close enough, or on your home network, can load firmware built for this project.

**The first time (one-time migration to the new layout):** this firmware has two app slots, which changes the partition
table. A board with older firmware needs it flashed once over USB: either the merged image at **0x0** (erases the saved
home Wi-Fi), or, to keep the Wi-Fi, two files: `a80-airprint-<id>-partition-table.bin` at **0x8000**, then the `-app.bin`
at **0x10000**. After that, update from the web page.

**Mixing web and USB updates:** the web page writes to the *other* slot, so after a web update the board may run slot 1.
Flashing an `-app` image over USB at 0x10000 always goes to slot 0, and the board keeps running slot 1 (the old firmware!)
until you flash the **merged** image, which also resets which slot boots. The status page and the console show
"Firmware slot" (`ota_0` or `ota_1`). So: after web updates, use the web page, or the merged image over USB.

In the web flasher leave "Erase all flash" off. If an update ever needs the merged image (the release notes say so),
set the home network up again afterwards.

Settings are in [`firmware/main/config.h`](firmware/main/config.h): hotspot name, printer name, the printer's BLE
name (`A80_BLE_NAME`; empty = the first A80 found), auto power-off, fanfold page calibration.

## Use

1. Switch on the printer and power the ESP32.
2. On the iPhone, join the Wi-Fi network **A80-Print** (no password, no internet), or, if you've set up a home
   network (below), just stay on your home Wi-Fi.
3. Share → Print → **A80 Thermal**. On Android, open the app's menu → Print and pick **A80 Thermal** (the phone must be on the
   same network as the printer; Android's built-in print service must be on, under Settings → Connected devices → Printing).

The serial console (115200) logs every step in plain English, with the printer traffic in hex.
The first line says which firmware is running, for example `firmware version 9999.99.99, commit 8c711ad`
(development builds use the placeholder version `9999.99.99`; releases carry their `YYYY.MM.NN` version). The
same appears on the status page under "Show Printer Web Page".

At boot, and again each time the printer connects, the console prints an **ESP32 information** block (firmware, build
date, ESP-IDF version, chip, MAC addresses, free internal RAM and PSRAM, flash and firmware space, why it last restarted,
uptime, Wi-Fi state), directly above the printer's own block. The status page has the same list under "ESP32". It
includes the MAC addresses, so mask them before pasting a log or page into an issue.

Every log line about a client (connections, requests, IPP operations, jobs) names the client's address, for example
`Client is checking printer status and settings [IPP Get-Printer-Attributes #3 from 10.1.1.68]`, so you can tell a phone
from a Mac. The address of a home network is private, but mask it too if you'd rather.

**After updating from an older firmware:** earlier builds printed on port 8631; this one uses port 80. A phone or
Mac that already knew the printer can keep the old address and fail with "The printer is no longer available" or a
preview that won't load. Cancel the stuck jobs and **restart the iPhone** (on a Mac, remove the printer and add it
again; this also refreshes the paper-type names).

## Home Wi-Fi (optional)

At home the printer can join your own Wi-Fi, so the iPhone prints without switching networks. Away from home (in the
car, say) it falls back to its own **A80-Print** hotspot.

**Set it up:** join the hotspot, open **Show Printer Web Page** (or `http://192.168.4.1/wifi`), pick your
network from the list (strongest signal first), enter the password and tap **Save**. The printer then tries the
network, and **only if it really joins is it saved**. The page that comes back shows the printer's **new address**
on your network (or **why it couldn't join**), and the hotspot stays on for 5 seconds, then turns off, which also moves your phone back to your home Wi-Fi by
itself. If it can't join (wrong password, out of range) nothing changes. The same page, reached on the home
network, changes the network later. No button on the ESP32 is involved.

**How it behaves:**
- At boot it tries the saved network for about 12 seconds, then starts the hotspot.
- If the home Wi-Fi drops, it reconnects for 20 seconds, then starts the hotspot and keeps looking for home every
  45 seconds, but only while no device is on the hotspot (looking scans other channels, which interrupts a transfer).
- **Forget the saved network:** the **Forget the saved network** button on the same page. The printer goes back to
  hotspot only.
- Nothing is required: with no home network saved it behaves as before.

**Security notes:** the hotspot is open, so anyone within range can join it, open the setup page and change or forget
the home network, and the password you enter crosses the air unencrypted. At home the same page is reachable by
anyone on your home network. The saved password is stored in the ESP32's flash in plain text and is never logged.
Flashing the merged image (at `0x0`) erases it; the `-app` image (at `0x10000`) keeps it.

## Releases

Releases are named `YYYY.MM.NN` (for example `2026.11.01`) and listed under **Releases**. Each one has these
images attached: the merged `a80-airprint-<version>.bin` (flash at **0x0**), `a80-airprint-<version>-app.bin`
(flash at **0x10000**, or upload on the printer's `/update` page, to update without losing the saved Wi-Fi) and
`a80-airprint-<version>-partition-table.bin` (0x8000, for the one-time move), no build needed.

To make a release (maintainers): **Actions → 🚀 Release → Run workflow** on `main`, and enter the version. It checks
the version format and that the tag doesn't exist yet, runs the same build every pull request must pass (stamped with
the version, which the firmware prints at boot), then creates the tag and the GitHub Release with generated notes.

## Repository

| Path | What |
|---|---|
| [`firmware/`](firmware/) | The ESP32-S3 firmware (ESP-IDF 5.5): hotspot, Bonjour, IPP server, URF → 1-bit converter, BLE printer driver. See [`firmware/README.md`](firmware/README.md) for design notes and history. |
| [`PROTOCOL.md`](PROTOCOL.md) | Everything known about the printer: job byte stream, `10 FF xx` commands and queries, status bits, BLE GATT layout and flow control, settings, battery, paper handling, what iOS displays. |
| [`DEVICE.md`](DEVICE.md) | Manufacturer, brands, chips, firmware availability. |
| [`docs/AIRPRINT.md`](docs/AIRPRINT.md) | What iOS needs from an AirPrint printer: Bonjour records, IPP operations and attributes, the URF format. |

## Cautions

- `PROTOCOL.md` lists commands from this printer family that factory-reset or update the firmware. **Never send
  them**: no firmware image is published, so a failed update can't be undone.
- The protocol was worked out from the vendor's drivers and by observation. It may differ on other firmware
  versions (tested: printer firmware V1.08).

## Licence and trademarks

MIT, see [LICENSE](LICENSE). Not affiliated with Apple, the printer's manufacturer or any of its brands.
AirPrint is a trademark of Apple Inc.
