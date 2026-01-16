?? Session Continuity Summary – Outdoor Cat Shelter Monitoring Project
Project Goal

Build a battery-powered data logger to monitor inside and outside temperature of a stationary outdoor cat shelter, with phone-based data download and no field charging.

Locked Design Decisions
Power

Battery system: 1S2P using two 18650 cells (3.0–4.2 V)

Charging: Indoors only

Field unit: No charging circuitry

Batteries are swapped, not charged in the enclosure

USB port used only for programming/debug

Data & Operation

Logs temperature data continuously

Must store at least 7–10 days of data

Uses external FRAM for storage

BLE data download required from day one

Phone downloads only new data since last offload

Data saved as CSV, plain text

Sensors

2× waterproof DS18B20 temperature probes

One inside shelter

One outside shelter

Direct wiring (no JST/STEMMA requirement)

Environment

Northeast U.S. climate

Components sheltered from direct weather exposure

User Constraints

User is not a programmer

All instructions must be:

Exact step-by-step

No explanations unless explicitly requested

Any code must be:

Fully written

Exact cut-and-paste

With explicit file locations

BOM Rules

Adafruit first, Mouser second

Direct product URLs only

Plain Excel BOM

No formulas

No formatting

Raw values only

Final Hardware Selected (Prototype)
Microcontroller

Adafruit Feather nRF52840 Express (PID 4062)

BLE-first

No Wi-Fi required

Chosen specifically for reliable phone BLE downloads

Storage

Adafruit SPI FRAM 256KB (PID 4718)

Timekeeping

DS3231 RTC breakout (PID 3013)

CR1220 backup battery (PID 380)

Temperature Sensors

2× Adafruit waterproof DS18B20 probes (PID 381)

Includes required pull-up resistor

Power / Wiring

Switched JST-PH breakout (PID 1863)

Silicone ribbon cable (PID 6181)

User already has:

18650 batteries

Chargers

Heat shrink

Misc. connectors

Current Status

All prototype parts have been ordered from Adafruit

No assembly or software steps have been performed yet

Waiting for parts to arrive

Next Planned Steps (After Parts Arrive)

Inventory check

Exact wiring steps (one wire at a time)

Exact software installation steps

Firmware upload (provided fully written)

First power-on verification

BLE download test to phone

CSV file verification

Possible Interim Step (Optional, Before Parts Arrive)

Create a Git repository to:

Organize firmware

Store BOM and documentation

Back up project state

End of summary.



When you’re ready:

Start a new chat

Paste everything above

Say either:

“Parts arrived”, or

“Let’s set up the Git repo first”

We’ll pick up cleanly from there.