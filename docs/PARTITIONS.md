# Partition table (8 MB flash)

`partitions.csv` (written by the lead/WP-28) with `CONFIG_PARTITION_TABLE_OFFSET=0x10000`.

| Name | Type | SubType | Offset | Size | Purpose |
|---|---|---|---|---|---|
| (bootloader) | — | — | 0x0 | < 0x10000 | ESP32-S3 second-stage bootloader, rollback enabled |
| (partition table) | — | — | 0x10000 | 0x1000 | |
| `nvs` | data | nvs | 0x11000 | 0x10000 (64 KiB) | settings, credentials, step history, drift, PHY calibration; 16 pages for wear spreading |
| `otadata` | data | ota | 0x21000 | 0x2000 | selects ota_0/ota_1; required for rollback |
| `coredump` | data | coredump | 0x23000 | 0x1D000 (116 KiB) | panic dumps (fills the gap up to the 64 KiB-aligned app start) |
| `ota_0` | app | ota_0 | 0x40000 | 0x300000 (3 MiB) | app slot A (v1 USB flashing writes here) |
| `ota_1` | app | ota_1 | 0x340000 | 0x300000 (3 MiB) | app slot B (future OTA) |
| `spare` | data | 0x40 | 0x640000 | 0x1C0000 (1.75 MiB) | reserved, unformatted (future assets/logs) so OTA never needs repartitioning |

Rules:
- App partitions are 64 KiB aligned; the image budget is 1.6 MiB (ARCH section 20), leaving
  > 45 % headroom per slot.
- No `factory` partition: with otadata present, `idf.py flash` writes `ota_0` + initial otadata.
- Rollback: the app calls `esp_ota_mark_app_valid_cancel_rollback()` after its first successful
  wake (no-op for USB-flashed images, which are not in PENDING_VERIFY).
- No `phy_init` partition (PHY data in NVS) and no `nvs_keys` (NVS encryption off; if Q-04 picks the
  HMAC scheme no key partition is needed; the flash-encryption scheme would need one, carved from
  `spare`).
- Recovery: `idf.py erase-flash` + `flash` restores everything; NVS loss = factory reset.
