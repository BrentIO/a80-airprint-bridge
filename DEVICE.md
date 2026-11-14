# A80 — who makes it, and firmware availability (web research 2026-11-12)

## Manufacturer
- **Real maker: Xiamen Print Future Technology Co., Ltd.** ("Future Print", est. 2018, Jimei District, Xiamen, China).
  Makes thermal printers, A4 portable printers, printer mechanisms and control boards. Site: https://www.futureprt.com (CN: futureprt.cn)
- **FCC ID 2A6FW-A80** (grantee 2A6FW = Xiamen Print Future), "Thermal Printer", granted 2023-06-01, 2402-2480 MHz,
  certified under two Part 15 rules (BLE/DTS + classic BT spread spectrum). Test lab Shenzhen ZKT. https://fccid.io/2A6FW-A80
  - Public exhibits: user manual, internal + external photos, test reports, antenna report. Block diagram, schematic and
    operational description are confidential. Internal photos returned 403 to automated fetches; open in a browser to see chips.
  - Same grantee has ~50 printers (C80, D2, Y486BT, L13, ST80, U8, ...).
- **Brands / resellers:** aixiqee, IOOIOO, FlashLabel and others sell the same A80. "YXWL" (USB vendor 0x5958) is the
  maker's driver/USB prefix (FlashLabel driver links are named YXWL-A4driver-*).
- **Support site (FlashLabel):** https://help.flashlabel.com/support/solutions/150000213895 (category "A80 & A80H & D80 & D80 Pro & C80 & ... & T8810").
  Contact on that site: Eddie@flashlabel.com — the same contact name as the FCC filing.
- **Companion app:** SharpFox (厉狐), also "FlashLabel" app.
- Sibling models sharing the A4 driver: A80H, A81/A81H, L81/L81H, C80, D80/D80 Pro, Y8/Y8 Pro, Y80, ST80K, T8810.

## Firmware updates
- **No firmware image, updater or release notes are published** on futureprt.com (downloads: drivers, manuals, apps only),
  help.flashlabel.com (driver/app install guides only), or anywhere else found. No mention of firmware/OTA in any manual found.
- The only plausible route is the SharpFox/FlashLabel phone app (if it has an update screen), or asking the vendor
  (FlashLabel support email) for an image + tool. Not confirmed that either exists.
- Our printer: firmware V1.08, boot loader 1.01, BT module _231113_r1389. A separate boot loader means the firmware is
  field-replaceable, and the 10 FF command family has update/OTA/boot-loader commands (see PROTOCOL.md NEVER SEND list).
  Without an official image and tool, flashing risks bricking it. Not recommended.

## Drivers published (versions)
- Linux amd64 1.2.1 (flashlabel.net/YXWL-A4driver-linux), macOS 1.2.0, Windows BT 1.3.0, Windows ARM 1.2.5,
  futureprt.com A80 Windows 1.2.3. (We analysed macOS pkg 1.2.7 / Linux .run 1.2.3 — newer than what's listed.)

## Sources
- https://fccid.io/2A6FW and https://fccid.io/2A6FW-A80
- https://www.futureprt.com/download.html
- https://help.flashlabel.com/support/solutions/articles/150000191214-install-driver-on-linux-via-usb-cable
- https://forums.linuxmint.com/viewtopic.php?t=459296 (another A80 owner; no firmware info)

## Chips (from the FCC internal photos, read from the photos)
- **Bluetooth: Barrot BR8051A01** (marking `BARROT BR8051A01 EK0S24 223204`). Barrot Technology (Shanghai) dual-mode
  Bluetooth SoC: its own MCU, SRAM and flash, BLE + classic. In this printer it's the wireless bridge with its own firmware
  (`_231113_r1389`); it talks to the main controller over UART with AT commands (`AT+LBDADDR?` at boot).
  `223204` is probably a date/lot code (perhaps 2022 week 32). No public datasheet; Feasycom sells modules built on the BR8051.
- **Main print controller: UNCONFIRMED.** Candidate: **Artery AT32F403AR** (marking `AT32F403AR… KNOK9-000`), read from the
  sharper internal photos of sibling model **2A6FW-C80**, which also shows the same Barrot BR8051A01. The A80's own photo is too
  blurry to confirm. The C80 is a smaller printer and may use a different board, so the A80's chip could be
  different. Only the Barrot BR8051A01 is confirmed for the A80 itself.
  - Artery Technology (Chongqing): ARM Cortex-M4F with FPU, up to 240 MHz, LQFP64 package (the "R"). Flash 256 KB–1 MB,
    depending on the letter after "R", which can't be read in the photo. It has USB full-speed (likely the printer's USB port).
  - It has a factory ROM boot loader (USB DFU / UART ISP, entered with the BOOT0 pin) plus SWD debug. The printer's own
    "boot loader 1.01" is most likely the maker's separate in-application updater on top of that.
  - Vendor firmware is normally read-protected (Artery "access protection"), so it can't be dumped, and none is published.
- **Firmware-version search (2026-11-12):** no public match anywhere for main firmware "V1.08" / boot loader "1.01" on these
  printers, or for the Bluetooth build "_231113_r1389". The AT32F403A is common in Chinese thermal printers: NIIMBOT
  B21 label printers use the AT32F403ARCT7 with a boot loader at the start of flash and the app at 0x08002000
  (https://printers.niim.blue/hardware/NIIMBOT-B21_C2B/niimbot-b21-c2b-unbrick/). Different maker; their firmware isn't
  interchangeable with ours.
- **Other sibling filings (2026-11-12):** the C801/ST80 filings look different from the A80 and two of them use a
  **Yichip YC3122** (marking `YC3122-1 C131218`). Yichip (Shanghai) makes it as a single-chip printer controller with built-in
  Bluetooth; NIIMBOT D110_M / D11_H use the YC3122-L. That's a different design from the A80 (separate Barrot Bluetooth
  chip), so it's not the A80's controller. The A80's main chip stays unidentified.
