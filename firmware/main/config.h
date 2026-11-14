#pragma once
// Wi-Fi hotspot (open network: no password)
#define A80_AP_SSID      "A80-Print"
#define A80_AP_CHANNEL   6
#define A80_AP_MAX_CONN  4

// Home Wi-Fi (optional; set up on the printer's /wifi page)
#define A80_HOME_JOIN_S        12   // at boot, and when trying a new network: wait this long for an address
#define A80_HOME_LOST_GRACE_S  20   // home network dropped: reconnect this long, then start the hotspot
#define A80_HOME_RETRY_S       45   // hotspot running: look for the home network this often (only if no phone is on it)

// AirPrint identity
// Printer to connect to: its exact BLE name (e.g. "A80_1A2B_LE"), or "" for the first A80 found.
#define A80_BLE_NAME     ""
#define A80_PRINTER_NAME "A80 Thermal"
// A80_VERSION and A80_COMMIT come from the build (version.cmake)
#ifndef A80_VERSION
#define A80_VERSION "9999.99.99"
#endif
#ifndef A80_COMMIT
#define A80_COMMIT "unknown"
#endif
#define A80_BRIDGE_VERSION A80_VERSION " (" A80_COMMIT ")"
#define A80_HOSTNAME     "a80printer"          // a80printer.local
#define A80_IPP_PORT     8631
#define A80_IPP_PORT_STR "8631"
#define A80_AP_IP        "192.168.4.1"
#define A80_UUID         "5b1e4a80-7f3c-4e2a-9a80-a80a80a80a80"

// Printer
#define A80_HEAD_DOTS    1664                  // print head width (dots, 203 dpi)

// Fanfold (continuous-feed Letter) mode, calibrated 2026-11-11 (one sheet = 279.4 mm):
// each page = FANFOLD_ROWS printed rows + TEAR_DOTS feed, so the next perforation stops at the tear bar.
// Drift tests: +64 -> +2 mm/page, +48 -> ~+1, +40 -> +2 (firmware, 2 pages); +32 chosen. ~1 mm jitter.
// 2026-11-12: +32 ran ~0.7 mm/page long over 6 mostly light pages (dark pages seem to advance a bit less); -> +27.
#define A80_FANFOLD_PITCH 2235                 // 279.4 mm sheet at 8 dots/mm
#define A80_TEAR_DOTS     120                  // feed after the last row: head -> tear bar
#define A80_FANFOLD_FIX   27
#define A80_FANFOLD_ROWS  (A80_FANFOLD_PITCH - A80_TEAR_DOTS + A80_FANFOLD_FIX)   // 2142
// Paper types, as people see them (logs, status page, and on the iPhone through the strings file served at
// /strings/en.strings). A job that doesn't name a paper type prints as the continuous perforated Letter.
#define A80_PAPER_FANFOLD_NAME "Continuous Perforated Letter"
#define A80_PAPER_ROLL_NAME    "Paper Roll"
// iPhone Quality "Normal" (the default) = automatic darkness: pages darker than this print at printer darkness 01,
// others at 02. Dense black at 02 makes the printer slow down and stutter, worst with a cold head (2026-11-12).
#define A80_AUTO_DARK_PCT 50
#define A80_AUTO_OFF_MIN  5                    // printer auto power-off (factory 20; user chose 5 so it is off ~10 min after the last print); set on connect if different
#define A80_WAIT_PRINTER_MIN 30  // a job waits this long for a switched-off printer, then is cancelled
#define A80_IDLE_POLL_MIN 5    // stop status/battery polling this long after connecting or printing
