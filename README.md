# Cat Shelter Monitor

Battery-powered temperature data logger to monitor temperatures and occupancy in feral cat shelters.

## Hardware (verified)
- Adafruit Feather nRF52840 Express
- DS3231 RTC + CR1220 backup
- 256KB SPI FRAM (CS = A1)
- 2x DS18B20 probes on 1-Wire (DATA = A2, external pull-up installed)
- 1S2P 18650 battery pack (not connected during bench testing)

## Firmware
- v1.0: Logging + BLE UART commands (STATUS / GETNEW / GETALL / CLEAR)

## Docs
See `docs/session-continuity.md`.

