"""Shared helpers for talking to the Digilent Analog Discovery 3 via the WaveForms SDK.

Loads the dwf shared library cross-platform and exposes open/close helpers.
"""

from __future__ import annotations

import ctypes
import sys
from ctypes import c_int, byref, create_string_buffer


def _load_dwf() -> ctypes.CDLL:
    if sys.platform.startswith("win"):
        return ctypes.cdll.dwf
    if sys.platform == "darwin":
        return ctypes.cdll.LoadLibrary("/Library/Frameworks/dwf.framework/dwf")
    return ctypes.cdll.LoadLibrary("libdwf.so")


dwf = _load_dwf()

HDWF_NONE = c_int(0)


def sdk_version() -> str:
    buf = create_string_buffer(32)
    dwf.FDwfGetVersion(buf)
    return buf.value.decode()


def open_device() -> c_int:
    """Open the first available Analog Discovery device. Exits on failure."""
    hdwf = c_int()
    if dwf.FDwfDeviceOpen(c_int(-1), byref(hdwf)) == 0 or hdwf.value == HDWF_NONE.value:
        err = create_string_buffer(512)
        dwf.FDwfGetLastErrorMsg(err)
        raise RuntimeError(f"Failed to open AD3: {err.value.decode().strip()}")
    return hdwf


def close_device(hdwf: c_int) -> None:
    if hdwf.value != HDWF_NONE.value:
        dwf.FDwfDeviceClose(hdwf)


def last_error() -> str:
    err = create_string_buffer(512)
    dwf.FDwfGetLastErrorMsg(err)
    return err.value.decode().strip()
