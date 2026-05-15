# Bench tests for the lake sensor

Two scripts that use the Digilent Analog Discovery 3 (AD3) and the WaveForms SDK
to verify the sensors before they get deployed.

| Script                    | What it does                                                                 |
|---------------------------|------------------------------------------------------------------------------|
| `test_ain420_i2c.py`      | AD3 acts as I2C master, scans the bus, reads the M5Stack U162 (AIN4-20mA).   |
| `capture_onewire.py`      | AD3 acts as logic analyzer, captures the 1-Wire bus driven by a XIAO.        |

## Prereqs

1. Install WaveForms (includes the dwf shared library):
   <https://digilent.com/shop/software/digilent-waveforms/>
2. Confirm Python can load the SDK:
   ```
   python -c "import ctypes; print(ctypes.cdll.dwf)"
   ```
   If that fails on Windows, add `C:\Program Files (x86)\Digilent\WaveFormsSDK\bin`
   (or wherever `dwf.dll` lives) to `PATH`.
3. No pip packages required -- ctypes only.

## Test 2: AIN4-20 over I2C

Wiring:

| AD3      | U162 (AIN4-20mA) |
|----------|------------------|
| V+ (3.3V or 5V) | VCC       |
| GND      | GND              |
| DIO 0    | SDA              |
| DIO 1    | SCL              |

Pull-ups: 4.7 kohm from SDA -> 3.3V and SCL -> 3.3V. **Many M5Stack Grove units
already include onboard pull-ups.** Run the script first -- if the bus scan
finds `0x55`, you're done. If it finds nothing, add the externals.

Connect the DFRobot pressure transducer to the U162's screw terminals per
M5Stack's pinout.

Run:
```
python test_ain420_i2c.py
```

Expected output:
```
I2C master ready (SCL=DIO1, SDA=DIO0, 100 kHz)
Scanning bus 0x08..0x77 ...
  found: 0x55
Reading register 0x20 every 1s. Ctrl-C to stop.
raw=0x9001 ( 400)    4.00 mA  ->  0.000 m
raw=0xb802 ( 696)    6.96 mA  ->  0.925 m
...
```

Sanity checks:
- ~4 mA at atmospheric pressure (gauge sensor, reads relative to atmosphere).
- Squeezing the diaphragm or submerging the probe should push the reading up.
- `[open loop?]` flag means the loop is broken or the transducer isn't powered.

The register layout (`0x20`, 2 bytes LE, value = mA * 100) matches what the
deployed firmware reads in [`src/main.cpp`](../src/main.cpp). If you ever swap
to a unit with a different register map, update both places.

## Test 3: 1-Wire capture (deployed firmware drives, AD3 sniffs)

Flash the deployed firmware in `dev` mode so it stays awake and re-runs the
sense cycle:

```
pio run -e dev -t upload
```

`dev` keeps USB CDC alive and skips deep sleep, so the XIAO will hit the 1-Wire
bus on every cycle. Tap the AD3 onto the bus and capture.

Wiring (AD3 side):

| AD3 pin   | Connected to                   |
|-----------|--------------------------------|
| DIO 0     | DS18B20 data line (D0 / GPIO 2 on the XIAO) |
| GND       | Common ground with XIAO        |

The DS18B20 sensors, pull-up, and XIAO are already wired per [WIRING.md](../WIRING.md).

Run:
```
python capture_onewire.py
```

The script arms a 32k-sample buffer at 10 MHz (3.2 ms window) and triggers on
the first falling edge of DIO0. Press the XIAO reset button (or just wait for
its next read cycle) to start a transaction.

Expected console output:
```
captured 32768 samples (3.28 ms)
low pulses found: 84
    0  t=  102.30 us  width=  485.40 us  RESET
    1  t=  600.80 us  width=  121.20 us  presence?
    2  t= 1024.10 us  width=    6.20 us  write-1 / read-slot
...
summary: 1 reset pulse(s), 1 presence-like pulse(s) -- bus is alive.
wrote onewire_capture.csv -- open in WaveForms (File > Open) or a plot tool.
```

A clean reset (>=480 us low) followed by a presence pulse (~100 us low) means
the sensors are answering. The XIAO's Serial Monitor handles the functional
check; this script verifies the protocol on the wire.

## Troubleshooting

- **"Failed to open AD3"**: WaveForms app is probably already connected. Close
  it (or release the device with `Settings > Device > Close`) and rerun.
- **Bus scan finds nothing**: pull-ups missing, wrong DIO pins, or unit not
  powered. Probe SDA/SCL with the AD3 scope -- they should idle high at 3.3V.
- **`I2C bus stuck (SDA held low)`**: a previous slave left SDA low. Power-cycle
  the U162 and rerun. The script tries `FDwfDigitalI2cClear` first to recover.
- **No falling edge trigger for 1-Wire capture**: the XIAO isn't transmitting,
  or its data pin isn't connected to DIO0. Confirm common ground.
