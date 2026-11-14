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
- **Paper:** roll, or continuous fanfold Letter (pages land on the perforations; choose "Continuous paper – Short
  Edge" under Media & Quality; the printer remembers your choice for the next job).
- **Quality:** Normal = automatic darkness (lighter for very dark pages, which print more cleanly), Best = darkest.
- **Status on the iPhone:** a line under the printer's name ("Ready - battery 80%", "Off - press power button",
  "Out of paper"), the battery under Printer Info → Supply Levels, and a status page under "Show Printer Web Page".
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

Every pull request is also built in GitHub Actions: download the merged image from the run's **Artifacts**
and flash it at **0x0**, no local build needed.

Settings are in [`firmware/main/config.h`](firmware/main/config.h): hotspot name, printer name, the printer's BLE
name (`A80_BLE_NAME`; empty = the first A80 found), auto power-off, fanfold page calibration.

## Use

1. Switch on the printer and power the ESP32.
2. On the iPhone, join the Wi-Fi network **A80-Print** (no password, no internet), or, if you've set up a home
   network (below), just stay on your home Wi-Fi.
3. Share → Print → **A80 Thermal**.

The serial console (115200) logs every step in plain English, with the printer traffic in hex.
The first line says which firmware is running, for example `firmware version 9999.99.99, commit 8c711ad`
(development builds use the placeholder version `9999.99.99`; releases carry their `YYYY.MM.NN` version). The
same appears on the status page under "Show Printer Web Page".

## Home Wi-Fi (optional)

At home the printer can join your own Wi-Fi, so the iPhone prints without switching networks. Away from home (in the
car, say) it falls back to its own **A80-Print** hotspot.

**Set it up:** join the hotspot, open **Show Printer Web Page** (or `http://192.168.4.1:8631/wifi`), pick your
network from the list (strongest signal first), enter the password and tap **Save**. The printer then tries the
network, and **only if it really joins is it saved**: the hotspot turns off and the printer shows up on your home
Wi-Fi. If it can't join (wrong password, out of range) nothing changes. The same page, reached on the home
network, changes the network later. No button on the ESP32 is involved.

**How it behaves:**
- At boot it tries the saved network for about 12 seconds, then starts the hotspot.
- If the home Wi-Fi drops, it reconnects for 20 seconds, then starts the hotspot and keeps looking for home every
  45 seconds, but only while no phone is on the hotspot (looking scans other channels, which interrupts a transfer).
- **Forget the saved network:** the **Forget the saved network** button on the same page. The printer goes back to
  hotspot only.
- Nothing is required: with no home network saved it behaves as before.

**Security notes:** the hotspot is open, so anyone within range can join it, open the setup page and change or forget
the home network, and the password you enter crosses the air unencrypted. At home the same page is reachable by
anyone on your home network. The saved password is stored in the ESP32's flash in plain text and is never logged.
A full firmware flash (the `0x0` image) erases it, along with the paper-mode setting.

## Releases

Releases are named `YYYY.MM.NN` (for example `2026.11.01`) and listed under **Releases**. Each one has the merged
firmware image attached (`a80-airprint-<version>.bin`): flash it at **0x0**, no build needed.

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
