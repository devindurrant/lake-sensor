"""Capture a XIAO-driven 1-Wire bus with the AD3 logic analyzer.

Use this to verify the protocol on the wire while the XIAO sketch
(OneWire + DallasTemperature) drives the DS18B20s.

Wiring:
    XIAO data pin -> DS18B20 data line (with 4.7 kohm pull-up to 3.3V)
    AD3 DIO 0     -> same data line  (sniffer only, do not drive)
    AD3 GND       -> common ground with XIAO

Captures 32k samples at 10 MHz (3.2 ms window), triggered on a
falling edge of DIO0 (the start of a master reset or write slot).

Outputs:
    onewire_capture.csv : time_us, level
    Console summary classifying each low pulse:
        reset (>=480 us), presence (~60-240 us), write-0 (>=15 us),
        write-1 (<15 us). Read slots and write-1 slots both start with
        a brief master pull-down; the response from the slave
        determines the bit -- inspect in WaveForms for full decode.
"""

from __future__ import annotations

import csv
from ctypes import c_int, c_ubyte, c_double, byref

from ad3_helpers import dwf, open_device, close_device, last_error, sdk_version

DIO_BUS = 0
SAMPLE_HZ = 10_000_000  # 10 MHz -> 100 ns per sample
N_SAMPLES = 32_768
TRIGGER_HOLDOFF_SAMPLES = 1024  # samples before the trigger
OUT_CSV = "onewire_capture.csv"

# Pulse-width thresholds in microseconds (standard speed 1-Wire).
RESET_MIN_US = 400.0
PRESENCE_MIN_US = 50.0
PRESENCE_MAX_US = 260.0
WRITE0_MIN_US = 15.0


def digital_in_setup(hdwf: c_int) -> float:
    dwf.FDwfDigitalInReset(hdwf)
    internal_hz = c_double()
    dwf.FDwfDigitalInInternalClockInfo(hdwf, byref(internal_hz))
    divider = max(1, int(round(internal_hz.value / SAMPLE_HZ)))
    actual_hz = internal_hz.value / divider
    dwf.FDwfDigitalInDividerSet(hdwf, c_int(divider))
    dwf.FDwfDigitalInSampleFormatSet(hdwf, c_int(8))  # 1 byte per sample (DIO0..7)
    dwf.FDwfDigitalInBufferSizeSet(hdwf, c_int(N_SAMPLES))
    dwf.FDwfDigitalInTriggerPositionSet(hdwf, c_int(N_SAMPLES - TRIGGER_HOLDOFF_SAMPLES))
    # trigsrcDetectorDigitalIn == 4
    dwf.FDwfDigitalInTriggerSourceSet(hdwf, c_ubyte(4))
    bit_mask = 1 << DIO_BUS
    # FDwfDigitalInTriggerSet(low, high, rise, fall)
    dwf.FDwfDigitalInTriggerSet(hdwf, c_int(0), c_int(0), c_int(0), c_int(bit_mask))
    return actual_hz


def capture(hdwf: c_int) -> tuple[list[int], float]:
    actual_hz = digital_in_setup(hdwf)
    dwf.FDwfDigitalInConfigure(hdwf, c_int(0), c_int(1))
    print(f"armed at {actual_hz/1e6:.2f} MHz, waiting for falling edge on DIO{DIO_BUS} ...")

    status = c_ubyte()
    # stsDone == 2
    while True:
        if dwf.FDwfDigitalInStatus(hdwf, c_int(1), byref(status)) == 0:
            raise RuntimeError(f"DigitalInStatus failed: {last_error()}")
        if status.value == 2:
            break

    buf = (c_ubyte * N_SAMPLES)()
    if dwf.FDwfDigitalInStatusData(hdwf, buf, c_int(N_SAMPLES)) == 0:
        raise RuntimeError(f"DigitalInStatusData failed: {last_error()}")

    # Extract DIO_BUS bit per sample.
    bit_mask = 1 << DIO_BUS
    levels = [1 if (b & bit_mask) else 0 for b in buf]
    return levels, actual_hz


def find_low_pulses(levels: list[int], sample_period_us: float) -> list[tuple[float, float]]:
    """Return list of (start_us, width_us) for every contiguous low run."""
    pulses: list[tuple[float, float]] = []
    n = len(levels)
    i = 0
    while i < n:
        if levels[i] == 0:
            start = i
            while i < n and levels[i] == 0:
                i += 1
            pulses.append((start * sample_period_us, (i - start) * sample_period_us))
        else:
            i += 1
    return pulses


def classify(width_us: float) -> str:
    if width_us >= RESET_MIN_US:
        return "RESET"
    if PRESENCE_MIN_US <= width_us <= PRESENCE_MAX_US:
        return "presence?"
    if width_us >= WRITE0_MIN_US:
        return "write-0"
    return "write-1 / read-slot"


def write_csv(levels: list[int], sample_hz: float, path: str) -> None:
    step_us = 1e6 / sample_hz
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["time_us", "DIO0"])
        for i, lv in enumerate(levels):
            w.writerow([f"{i * step_us:.3f}", lv])


def main() -> None:
    print(f"WaveForms SDK {sdk_version()}")
    hdwf = open_device()
    try:
        levels, actual_hz = capture(hdwf)
        step_us = 1e6 / actual_hz
        pulses = find_low_pulses(levels, step_us)
        print(f"captured {len(levels)} samples ({len(levels) * step_us / 1000:.2f} ms)")
        print(f"low pulses found: {len(pulses)}\n")
        for idx, (t, w) in enumerate(pulses[:24]):
            print(f"  {idx:3d}  t={t:8.2f} us  width={w:7.2f} us  {classify(w)}")
        if len(pulses) > 24:
            print(f"  ... {len(pulses) - 24} more")

        write_csv(levels, actual_hz, OUT_CSV)
        print(f"\nwrote {OUT_CSV} -- open in WaveForms (File > Open) or a plot tool.")
        resets = [w for _, w in pulses if w >= RESET_MIN_US]
        presence = [w for _, w in pulses if PRESENCE_MIN_US <= w <= PRESENCE_MAX_US]
        if resets and presence:
            print(f"summary: {len(resets)} reset pulse(s), {len(presence)} presence-like pulse(s) -- bus is alive.")
        elif resets and not presence:
            print("summary: master reset detected but no presence pulse -- check pull-up and sensor power.")
        else:
            print("summary: no clear reset pulse -- did the XIAO sketch run during capture?")
    finally:
        close_device(hdwf)


if __name__ == "__main__":
    main()
