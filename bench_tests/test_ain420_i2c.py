"""Bench-test the M5Stack U162 (AIN4-20mA) over I2C using the AD3 as master.

Wiring:
    AD3 V+ (3.3V or 5V) -> U162 VCC
    AD3 GND             -> U162 GND
    AD3 DIO 0           -> U162 SDA
    AD3 DIO 1           -> U162 SCL
    4.7 kohm pull-ups from SDA and SCL to 3.3V (skip only if the Grove unit's
    onboard pull-ups are sufficient -- the bus scan will tell you).

Register layout (matches lake_sensor_firmware.ino):
    0x20: 2 bytes little-endian, value = current_mA * 100
"""

from __future__ import annotations

import time
from ctypes import c_int, c_ubyte, c_double, byref

from ad3_helpers import dwf, open_device, close_device, last_error, sdk_version

# ---- I2C pin / speed ----
SDA_DIO = 0
SCL_DIO = 1
I2C_HZ = 100_000

# ---- U162 (AIN4-20mA) ----
U162_ADDR = 0x55
U162_CURRENT_REG = 0x20  # 2 bytes LE, value = mA * 100

# ---- Depth mapping (matches firmware) ----
DEPTH_MIN_M = 0.0
DEPTH_MAX_M = 5.0


def i2c_setup(hdwf: c_int) -> None:
    dwf.FDwfDigitalI2cReset(hdwf)
    dwf.FDwfDigitalI2cStretchSet(hdwf, c_int(1))
    dwf.FDwfDigitalI2cRateSet(hdwf, c_double(I2C_HZ))
    dwf.FDwfDigitalI2cSclSet(hdwf, c_int(SCL_DIO))
    dwf.FDwfDigitalI2cSdaSet(hdwf, c_int(SDA_DIO))
    nak = c_int()
    if dwf.FDwfDigitalI2cClear(hdwf, byref(nak)) == 0 or nak.value == 0:
        raise RuntimeError(f"I2C bus stuck (SDA held low). nak={nak.value} err={last_error()}")


def i2c_scan(hdwf: c_int) -> list[int]:
    """Return list of 7-bit addresses that ACK'd."""
    found: list[int] = []
    nak = c_int()
    for addr in range(0x08, 0x78):
        # Zero-byte write probes presence without disturbing register state.
        if dwf.FDwfDigitalI2cWrite(hdwf, c_int(addr << 1), None, c_int(0), byref(nak)) == 0:
            continue
        if nak.value == 0:
            found.append(addr)
    return found


def i2c_read_register(hdwf: c_int, addr: int, reg: int, n: int) -> bytes:
    """Write `reg`, then read `n` bytes. Raises on NAK."""
    tx = (c_ubyte * 1)(reg)
    rx = (c_ubyte * n)()
    nak = c_int()
    ok = dwf.FDwfDigitalI2cWriteRead(
        hdwf,
        c_int(addr << 1),
        tx, c_int(1),
        rx, c_int(n),
        byref(nak),
    )
    if ok == 0:
        raise RuntimeError(f"I2C transaction failed: {last_error()}")
    if nak.value != 0:
        raise RuntimeError(f"I2C NAK at byte {nak.value} (addr=0x{addr:02x} reg=0x{reg:02x})")
    return bytes(rx)


def ma_to_depth(ma: float) -> float:
    """Map 4-20 mA linearly to DEPTH_MIN_M..DEPTH_MAX_M. Clamped outside range."""
    if ma <= 4.0:
        return DEPTH_MIN_M
    if ma >= 20.0:
        return DEPTH_MAX_M
    return DEPTH_MIN_M + (ma - 4.0) * (DEPTH_MAX_M - DEPTH_MIN_M) / 16.0


def main() -> None:
    print(f"WaveForms SDK {sdk_version()}")
    hdwf = open_device()
    try:
        i2c_setup(hdwf)
        print(f"I2C master ready (SCL=DIO{SCL_DIO}, SDA=DIO{SDA_DIO}, {I2C_HZ/1000:.0f} kHz)")

        print("Scanning bus 0x08..0x77 ...")
        addrs = i2c_scan(hdwf)
        if not addrs:
            print("  no devices found. Check pull-ups, power, wiring.")
            return
        print("  found:", ", ".join(f"0x{a:02x}" for a in addrs))
        if U162_ADDR not in addrs:
            print(f"  WARNING: U162 expected at 0x{U162_ADDR:02x} but not present.")
            return

        print(f"\nReading register 0x{U162_CURRENT_REG:02x} every 1s. Ctrl-C to stop.\n")
        while True:
            try:
                data = i2c_read_register(hdwf, U162_ADDR, U162_CURRENT_REG, 2)
                raw = int.from_bytes(data, "little", signed=False)
                ma = raw / 100.0
                depth = ma_to_depth(ma)
                flag = ""
                if ma < 3.5:
                    flag = "  [open loop?]"
                elif ma > 21.0:
                    flag = "  [overrange/fault?]"
                print(
                    f"raw=0x{data.hex()} ({raw:5d})  "
                    f"{ma:6.2f} mA  ->  {depth:5.3f} m{flag}"
                )
            except RuntimeError as exc:
                print(f"  read failed: {exc}")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        close_device(hdwf)


if __name__ == "__main__":
    main()
