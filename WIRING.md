# Lake Sensor — Hardware Wiring & Component Reference

## Overview

The lake sensor node measures water temperature at up to four depths and water level (depth) via a 4-20 mA pressure transducer. All data is transmitted over WiFi to Home Assistant via an HTTPS webhook every 15 minutes. Between transmissions the ESP32-C3 is in deep sleep to conserve power.

```
┌─────────────────────────────────────────────────┐
│           Seeed XIAO ESP32-C3                   │
│                                                 │
│  D0 ──────────────── 1-Wire bus ──┬── DS18B20 #1 (surface)
│                                   ├── DS18B20 #2 (mid)
│                                   ├── DS18B20 #3 (deep)
│                                   └── DS18B20 #4 (bottom)
│                                                 │
│  D4 (SDA) ─────────┐                            │
│  D5 (SCL) ─────────┴─ I2C bus ── M5Stack U162   │
│                                       │         │
│                               4-20 mA loop      │
│                            Pressure Transducer  │
└─────────────────────────────────────────────────┘
```

---

## Bill of Materials

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | Seeed Studio XIAO ESP32-C3 | Main MCU, 3.3 V logic |
| 4 | DS18B20 waterproof temperature probe | 1-Wire; waterproof stainless-steel probe variant recommended for submersion |
| 1 | M5Stack U162 4-20 mA to I2C Unit | Handles 4-20 mA signal conditioning and I2C conversion |
| 1 | 4-20 mA pressure transducer (0–5 m range) | e.g. DFRobot SEN0257 or equivalent submersible type |
| 1 | 4.7 kΩ resistor | 1-Wire pull-up: between DQ line and 3.3 V |
| — | ~~165 Ω resistor~~ | Not needed — shunt is internal to the U162 |
| 1 | LiPo battery or regulated 3.3 V supply | Size to duty cycle; 15-min sleep interval is battery-friendly |
| 1 | Waterproof enclosure (IP67 or better) | House the MCU and U162; route sensor cables through cable glands |
| — | Hookup wire / JST connectors | |

---

## GPIO Pin Assignments

| Function | XIAO Label | GPIO # | Notes |
|----------|------------|--------|-------|
| 1-Wire data bus | D0 | GPIO 2 | All DS18B20 sensors share this single wire |
| I2C SDA | D4 | GPIO 6 | Shared I2C bus — connect to U162 SDA |
| I2C SCL | D5 | GPIO 7 | Shared I2C bus — connect to U162 SCL |

> **Important:** On the XIAO ESP32-C3, the D-pin numbers printed on the board do **not** match GPIO numbers. Always wire to the **D-pin label** and use the GPIO numbers above in firmware.

---

## DS18B20 Temperature Sensors (1-Wire)

Up to four DS18B20 probes share a single 1-Wire bus on **D0 (GPIO 2)**. They are addressed by their unique 8-byte ROM ID; the firmware maps each ROM to a fixed depth label (surface/mid/deep/bottom) so wiring order is irrelevant.

### Wiring (external power mode — recommended)

```
3.3V ──┬──── 4.7 kΩ ────┬──────── DS18B20 VDD (red)
       │                │
       │               DQ ──────── DS18B20 DQ  (yellow)  ─── D0 (GPIO 2)
       │
      GND ────────────────────── DS18B20 GND (black)
```

> **Parasitic power** (2-wire mode, VDD tied to GND) also works but is less reliable at 12-bit resolution. External power is preferred.

### Notes
- All four sensors connect in parallel on the same DQ line.
- Run the data wire in a twisted pair with GND to reduce noise on long cable runs.
- Resolution is set to **12-bit** in firmware (~0.0625 °C precision, ~750 ms conversion time).
- Sensors are labelled by index in the JSON payload: `temp1_c` … `temp4_c`. The index maps to bus enumeration order — note which physical probe is at which depth.

---

## M5Stack U162 + Pressure Transducer (I2C / 4-20 mA)

The U162 module converts the 4-20 mA loop current from the pressure transducer into a 12-bit digital reading accessible over I2C.

### I2C Wiring

```
XIAO D4 (GPIO 6) ── SDA ── U162 SDA
XIAO D5 (GPIO 7) ── SCL ── U162 SCL
3.3V             ── VCC ── U162 VCC
GND              ── GND ── U162 GND
```

- **I2C address:** `0x55`
- **ADC data register:** `0x20` (16-bit little-endian; lower 12 bits are valid)

### 4-20 mA Loop Wiring

```
3.3V ──────────────────────────────────── Transducer V+ (supply)
                                                │
                                          Transducer
                                         (varies 4-20 mA)
                                                │
Transducer signal out ──────────────── U162 IN+
U162 IN-             ──────────────── GND
```

> The U162 contains an internal shunt resistor and STM32 microcontroller that measure the loop current and compute the result on-board. **No external shunt resistor is needed.** The 165 Ω figure from an earlier firmware draft was incorrect and has been removed.

---

## Power

| Parameter | Value |
|-----------|-------|
| MCU supply voltage | 3.3 V |
| Deep sleep current (ESP32-C3) | ~5 µA typical |
| Wake interval | 15 minutes |
| Active wake duration | ~3–5 seconds (sensor read + WiFi + HTTPS) |

A 3.7 V LiPo with a 3.3 V LDO regulator will power the node for months depending on capacity. WiFi connection is the dominant power consumer during each wake cycle.

---

## Signal Conversion Reference

These formulas are implemented in `src/main.cpp` and are provided here for verification and calibration.

### U162 I2C Protocol

The U162's internal STM32 performs all signal conditioning. The ESP32 reads two registers over I2C:

| Register | Address | Contents |
|----------|---------|----------|
| `CURRENT_REG` | `0x20` | `current_mA × 100` as uint16_t little-endian. e.g. `400` = 4.00 mA, `2000` = 20.00 mA |
| `ADC_REG` | `0x00` | Raw 16-bit averaged ADC count (diagnostic use only) |

### I2C Read Sequence (per register)

```
1. Write register address byte to 0x55
2. Repeated-start (endTransmission(false))
3. requestFrom(0x55, 2)
4. Read LSB, read MSB
5. current_mA = uint16(MSB<<8 | LSB) / 100.0
```

### Current → Depth (meters)

Linear mapping of the 4-20 mA range to 0–5 m:

```
depth_m = ((I_mA - 4.0) / (20.0 - 4.0)) × 5.0
```

Clamped to `[0.0, 5.0]` m. Adjust `DEPTH_MAX_M` in `src/main.cpp` if your transducer has a different range.

| Current | Depth |
|---------|-------|
| 4 mA | 0.0 m (sensor minimum / dry) |
| 8 mA | 1.25 m |
| 12 mA | 2.5 m |
| 16 mA | 3.75 m |
| 20 mA | 5.0 m (sensor maximum) |
