# Session Continuity – Cat Shelter Monitor  
_Last updated: 2026-01-15_

## Project Overview
Battery-powered temperature data logger for an outdoor cat shelter.

- Logs **inside and outside temperature**
- Uses **FRAM for storage**
- **BLE download to phone**
- **CSV output**
- No field charging; batteries swapped indoors
- Target users are **non-technical**

---

## Hardware Status (COMPLETE & VERIFIED)

### Microcontroller
- Adafruit Feather nRF52840 Express

### Storage
- SPI FRAM 256KB
- CS pin: **A1**

### Timekeeping
- DS3231 RTC (I2C)
- CR1220 coin cell installed (RTC backup only)

### Sensors
- 2× DS18B20 waterproof probes
- Shared 1-Wire bus
- DATA pin: **A2**
- External pull-up resistor installed (DATA ? 3V)

### Power
- 1S2P 18650 battery pack
- Battery **not connected during bench testing**
- USB power verified
- Coin cell powers RTC only

### Wiring
- All wiring confirmed correct
- USB power test successful
- No electrical issues outstanding

---

## Software Status

### Firmware v1.0 (BASELINE – LOCKED)
- Logs temperature every **1 minute**
- Stores records in FRAM
- BLE UART commands:
  - `STATUS`
  - `GETNEW`
  - `GETALL`
  - `CLEAR`
- CSV format verified
- BLE + logging confirmed working end-to-end

### Known UX Issues in v1.0
- Generic BLE terminal required
- Manual copy/paste of scrolling text
- Buffer truncation in some apps
- Commands are non-intuitive for end users

These issues are **UX-related only**, not hardware or data integrity problems.

---

## GitHub / Version Control Status (IMPORTANT)

### Repository
- GitHub repo: `RocketsCatShelterMonitor`
- Remote correctly configured
- `main` branch is clean and synced

### Tags
- **v1.0** tag created and pushed  
  - Represents: *Hardware verified + original firmware behavior*

### Branching
- Active development branch:

---

## v1.1-ble-usability

- All future firmware changes must happen **only** on this branch

---

## Planned Improvements (v1.1 – NOT YET IMPLEMENTED)

Chosen improvement strategy:
- **Path 1 + Path 2**
- Self-guided BLE output
- Chunked CSV transfer

### v1.1 Design Goals
- Auto status/help text on BLE connect
- Replace multiple commands with a single user-facing command:

---

## Download

- Send **pure CSV only**
- Chunk output to avoid buffer overruns
- End with a clear marker:

---

## DOWNLOAD_COMPLETE,<sent>,<remaining>

- Keep logging interval at 1 minute
- Preserve all existing pin assignments

### Important Constraint
User is **not a programmer**.  
Any future code changes must be:
- One complete file
- Copy/paste only
- Exact file locations specified
- Step-by-step instructions, no assumptions

---

## Where to Resume Next Session

### First action tomorrow
1. Open this repository
2. Confirm you are on branch:

---

## v1.1-ble-usability

3. Provide the **current firmware HELP or STATUS output**
so the v1.1 code can be generated without guessing

### Next milestone
- Create `CatShelterLogger_v1_1.ino`
- Implement BLE usability improvements
- Commit as first v1.1 change
- Keep v1.0 untouched and recoverable

---

## Current State Summary
- Hardware: ? complete
- Firmware v1.0: ? working, tagged
- GitHub: ? clean, synced, recoverable
- UX improvements: ? planned, not yet started

Project is stable and ready to continue.
