# Cat Shelter Monitor — Field Battery Circuit (Final)

Battery packs are NOT charged in the field. Batteries are removed from the field enclosure,
charged in the office, and returned to the field.

The battery enclosure feeds the Feather through a 3 ft 2-wire cable into the Feather JST battery connector.

## Components inside battery enclosure

- 1-cell Li-ion battery
- Protection module (terminals: B+, B-, P+, P-)
- RUEF110 resettable PTC fuse (1.1A hold)
- On/Off switch
- 3 ft 22 AWG two-wire cable to Feather JST battery input

## Wiring

Battery → Protection board

Battery + → B+
Battery - → B-

Protected output → Feather

P+ → RUEF110 PTC → On/Off switch → Cable +
P- → Cable -

Cable + → JST +
Cable - → JST -

## Design Notes

- Protection board remains permanently connected to battery.
- Switch interrupts load only, not protection circuit.
- PTC fuse protects wiring and Feather from shorts.
- Cable shortened from 9 ft to 3 ft to reduce voltage drop.
- Wire gauge: 22 AWG.

## Field Check

Before connecting to the Feather:

Switch ON  
Measure voltage at JST connector

Red probe → JST +  
Black probe → JST -

Expected voltage: ~3.7–4.2 V

If voltage reads negative polarity is reversed.
