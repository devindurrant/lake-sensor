# TLS CA bundle

`x509_crt_bundle.bin` is the Mozilla root CA set in ESP-IDF's compact bundle
format. The firmware validates the `hooks.nabu.casa` TLS certificate against
it (`WiFiClientSecure::setCACertBundle`), so the device keeps working if Nabu
Casa rotates certificate authorities -- no single hand-pinned root to break.

## Why this exists as a committed binary

The Arduino IDE embeds this bundle automatically; PlatformIO does not. So the
bundle is generated once, committed, and embedded by `platformio.ini`:

```ini
board_build.embed_files =
  cert/x509_crt_bundle.bin
```

PlatformIO turns that path into the linker symbol
`_binary_cert_x509_crt_bundle_bin_start`, which `src/main.cpp` declares:

```cpp
extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_cert_x509_crt_bundle_bin_start");
```

If you move or rename the file, update both the `embed_files` path and that
`asm(...)` symbol to match (path separators and dots become underscores).

## Regenerating (refresh the CA set)

Roots change slowly, but to refresh:

```bash
# 1. Official ESP-IDF generator (match the IDF version arduino-esp32 uses)
curl -fsSL -o gen_crt_bundle.py \
  https://raw.githubusercontent.com/espressif/esp-idf/v5.1.4/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py

# 2. Mozilla CA set (curl project's maintained export)
curl -fsSL -o cacert.pem https://curl.se/ca/cacert.pem

# 3. Generate (needs Python + `pip install cryptography`)
python gen_crt_bundle.py --input cacert.pem    # writes ./x509_crt_bundle

# 4. Replace the committed copy
mv x509_crt_bundle cert/x509_crt_bundle.bin
```

Then `pio run` and confirm the link succeeds. The bundle is public data (no
secrets); committing it keeps builds reproducible without network access.
