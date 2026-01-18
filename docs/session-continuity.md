Session Continuity – Cat Shelter Monitor

Last updated: 2026-01-18

CRITICAL: If your instructions become inaccurate, ambiguous, or rely on unverified UI behavior, STOP, restate my constraints, and reset before continuing.

1. Check out repository: RocketsCatShelterMonitor
2. Switch to branch: v1.1-demo-ram-buffer
3. Flash firmware to Adafruit Feather nRF52840 Express
4. Power via USB
5. Connect via BLE UART app
6. Type: STATUS

Project Overview

Battery-powered temperature data logger for an outdoor cat shelter.

Logs inside and outside temperature

RTC-based absolute timestamps

BLE download to phone

CSV output

Designed for non-technical users

Field power via battery swap (no charging in enclosure)

IMPORTANT – User Interaction Constraints (DO NOT IGNORE)

The project owner is NOT a programmer.
All assistance must follow these rules:

? No assumptions about prior knowledge

? Step-by-step instructions only

? Exact copy/paste code blocks (complete files, not fragments)

? Exact file paths and filenames

? No “just change X” instructions

? No implied steps

? No unexplained jargon

? Verify instructions before presenting them

If a step is ambiguous or risky, it must be called out explicitly.

This constraint is as important as hardware correctness.

Hardware Status
Microcontroller

Adafruit Feather nRF52840 Express

Temperature Sensors

2× DS18B20 waterproof probes

Shared 1-Wire bus

DATA pin: A2

External 4.7 k? pull-up resistor (A2 ? 3.3V)

Both probes verified working simultaneously

Timekeeping

DS3231 RTC

I²C (SDA / SCL)

Coin cell installed

RTC verified working

rtc_unix confirmed valid and incrementing

Storage

FRAM temporarily removed

FRAM troubleshooting paused pending replacement module

Power

USB power for bench testing

3.3V rail verified stable

Common ground confirmed

Wiring Notes (Important Lessons Learned)

Breadboard rail continuity matters

DS18B20 failures were caused by loose Dupont connections

DS18B20 85.00°C reading identified as a wiring/contact fault

DS18B20 -127 / nan identified as bus not detecting devices

Final wiring confirmed stable

Firmware Status
v1.0 (Baseline – Locked)

FRAM-based logging

BLE UART commands:

STATUS

GETNEW

GETALL

CLEAR

CSV format verified

Hardware + firmware validated

Tagged and preserved

v1.1 Demo (RAM-Based – Temporary)

Branch: v1.1-demo-ram-buffer
Purpose: Proof-of-concept demo while FRAM hardware unavailable

Key Characteristics

Logs to RAM only (volatile)

No FRAM access

Explicitly reports ram_only=1

Data lost on reset (expected)

Logging

Interval: 1 minute

Capacity: 1440 records (~24 hours)

BLE UX Improvements

Auto welcome text on connect

Simplified commands:

STATUS

DOWNLOAD (since last download)

DOWNLOADALL

CLEAR

Chunked CSV transfer

Clear end marker:

DOWNLOAD_COMPLETE,<sent>,<remaining>

CSV Format (Demo)
ts_unix,t0C,t1C
1768616616,20.69,19.75

Verified at End of Session

ds18b20_count=2

Both probes report realistic temperatures

RTC timestamps valid

BLE transfer stable

Android (Galaxy S10) tested successfully

GitHub / Version Control Status
Repository

RocketsCatShelterMonitor

Remote synced

Clean history preserved

Branches

main – stable

v1.0 – tagged baseline

v1.1-ble-usability – FRAM-based future work

v1.1-demo-ram-buffer – temporary RAM demo (current working demo)

Recent Commits

Checkpoint commit saved before RAM demo

Demo branch contains known-good RTC + DS18B20 + BLE state

Known Issues / Deferred Work
FRAM

Original FRAM module unreliable on breadboard

Replacement FRAM pending

FRAM reintegration deferred until hardware arrives

Demo Limitations

RAM-only storage

Data lost on reset

Shortened CSV (intentional for BLE reliability)

Next Steps (When Ready)

Receive replacement FRAM module

Reintroduce FRAM on v1.1-ble-usability

Restore persistent logging

Merge BLE UX improvements from demo branch

Remove demo-only flags (ram_only, short CSV)

Tag final v1.1 release

Current State Summary

Hardware: ? RTC + 2× DS18B20 verified

Firmware: ? RAM demo stable and demo-ready

BLE UX: ? usable for non-technical users

FRAM: ? pending replacement

GitHub: ? clean, recoverable, well-branched

Project is stable, demo-ready, and correctly staged for FRAM reintegration.

PCB v1 Carrier (KiCad 9.0.7) – Schematic Progress
- Feather headers, FRAM socket, RTC socket, 2x probe terminals, and expansion header are placed and wired.
- Expansion header nets wired: +3V3, GND, A3, A4, D5, D6.
- ERC: All real connectivity issues resolved.
- ERC power-source complaints for carrier-board power (external from Feather) were excluded with comments:
  "Carrier board: power supplied externally by Feather"
  Excluded items included: Pin not connected / Input power not driven related to power symbols/flags.
- Next step: Assign footprints and then update PCB layout from schematic.
