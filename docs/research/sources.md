# Sources (R1 hardware research)

Retrieval date for every entry: **2026-10-05**. "Used" = a fact in hardware.md / ssd1681.md / bma423.md depends on it. "Read-only" web access:
nothing was installed or executed from the web. No third-party driver/library source was read (the Watchy library `config.h` that SQFMI's docs link to
was deliberately **not** opened; the BMA423 mirror was read for licence text, headers and a blob byte count only, see the BMA423 section).

## SQFMI hardware and documentation
| URL | Note |
|---|---|
| https://watchy.sqfmi.com/docs/hardware | Used. Official hardware page (via fetch summary): v3 revision table, datasheet list (ESP32-S3, GDEY0154D67, BMA423, LDO ME6211, battery, motor), schematic/BOM links |
| https://watchy.sqfmi.com | Home page; navigation only, no hardware detail (shows the docs live under /docs) |
| https://github.com/sqfmi/watchy-hardware | Used. Official hardware repo: tags v1.0/v1.5/v2.0, branch v3.0 (commit a31e74ec56, 2026-01-31), main 5f147972aa, MIT licence (c) 2026 SQFMI |
| https://github.com/sqfmi/watchy-hardware/blob/v3.0/WatchySchematic.pdf | Used. v3.0 schematic (1 sheet, KiCad 8.0.4, Rev 3.0; raw download via /raw/v3.0/) |
| https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/Watchy.kicad_sch | Used. KiCad schematic source; netlist extracted by a local script and cross-checked against the PDF |
| https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/Watchy.kicad_pcb | Used. PCB (KiCad 7 format): component positions, BMA423 orientation, button positions, board outline |
| https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/WatchyBOM.csv | Used. BOM; disagrees with the schematic on several passives (see hardware.md 1) |
| https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/README.md and .../main/LICENSE | Repo readme and MIT licence text |
| https://api.github.com/users/sqfmi/repos, .../repos/sqfmi/watchy-hardware/{git/trees/main?recursive=1,tags,branches,commits} | Repo listings and commit history (which files exist, when v3 sources were uploaded) |
| https://watchy.sqfmi.com/interactive_bom/watchy_interactive_bom.html | Fetched and decoded; **stale** (dated 2021-12-26, ESP32 v1.x board); not used for facts |
| https://github.com/sqfmi/watchy-docs (API tree) and raw docs `docs/hardware.md`, `docs/battery-life.md`, `docs/faqs.md`, `docs/getting-started.md` (https://raw.githubusercontent.com/sqfmi/watchy-docs/main/docs/...) | Used. Official docs: revision table (v3: no external RTC, USB CDC/JTAG, buttons for reset), battery-life claims (5-7 days timekeeping), FAQ (display front/bezel), getting-started (BACK+UP 4 s bootloader/reset procedure) |
| https://github.com/sqfmi/Watchy/blob/master/src/config.h | Linked by SQFMI as the pin map; **not read** (third-party library source, rule 2) |
| https://watchy.sqfmi.com/pdf/BST-BMA423-DS000-1509600.pdf | Used. SQFMI-hosted Bosch BMA423 datasheet rev 1.1 (May 2019, preliminary, 129 pp.); features, remap table, axis figure |

## Espressif
| URL | Note |
|---|---|
| https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf | Used. ESP32-S3 datasheet v2.2 (87 pp.): pin tables, RTC GPIO/ADC maps, strapping, ADC ranges, DC, deep-sleep current, ESP32-S3FN8 comparison |
| https://www.espressif.com/sites/default/files/documentation/esp32-s3_hardware_design_guidelines_en.pdf | Returned an HTML "moved" page; not used (replaced by the online guidelines below) |
| https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html | Used. 32.768 kHz crystal sizing/ESR, CHIP_PU RC (10K/1uF), GPIO0 pull-up guidance |
| https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html | Used (fetch summary, ESP-IDF v6.1 docs): EXT1 GPIOs 0-21, ANY_LOW/ANY_HIGH, RTC memory retention |
| https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-serial-jtag-console.html | Used (fetch summary, v6.1): GPIO19/20, deep-sleep disconnect, /dev/ttyACM, download mode |
| https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc_oneshot.html (redirects to .../peripherals/adc/adc_oneshot.html) | Used lightly: ADC2/Wi-Fi arbitration note; refers to the datasheet for attenuation ranges |

## Board components
| URL | Note |
|---|---|
| https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6128/TP4054-5272.pdf | Used. TP4054 charger datasheet: CHRG open-drain behaviour, PROG table (10K = 100 mA), thresholds |
| https://www.richtek.com/assets/product_file/RT9080/DS9080-09.pdf | Used. RT9080 LDO: Iq 2 uA, dropout 0.31 V at 600 mA, 1.1 A limit (schematic part RT908033) |
| https://cdn.components.ru/media/PDF/2180717.pdf | Used. Mirror of ST **SR1** datasheet (DocID026048): ordering-code decode (H = 4.0 s, A = open-drain active-low, R = no tREC) used for the sibling SR2HARU |
| https://www.st.com/resource/en/datasheet/sr2.pdf ; https://www.st.com/en/reset-and-supervisor-ics/sr2.html | SR2 datasheet/product page: download failed / fetch timed out (HTTP/2 error, 60 s timeout). SR2 facts (6-pin, both SR inputs must be asserted, 1.5 uA) come from search-result summaries only |
| https://www.powerstream.com/lip/GMB042030.pdf | Used. GM Battery GMB042030 cell spec: 180 mAh typ, 4.2 V, 0.5C = 90 mA, bare cell |
| https://github.com/SeeedDocument/Bazaar_doc/raw/master/316040001/1020_datasheet.doc | Used. Vibration motor 1020-15-003-001: 3.0 V rated, <= 80 mA, start <= 2.3 V (text extracted from the .doc) |
| https://datasheet.lcsc.com/szlcsc/Nanjing-Micro-One-Elec-ME6211C33M5G-N_C82942.pdf | Listed by SQFMI docs as the LDO; download returned HTML, not read |

## E-paper (SSD1681 / GDEY0154D67)
| URL | Note |
|---|---|
| https://support.newhavendisplay.com/hc/en-us/article_attachments/41742572114967 | Used. Solomon Systech SSD1681 datasheet **Rev 1.1, Apr 2021** (43 pp.), copy hosted by Newhaven Display: commands, RAM, timing, currents |
| https://cdn.shopify.com/s/files/1/0264/7629/files/SSD1681.pdf | Older SSD1681 v0.13 (2019) copy; searched for display-mode-2 text (absent), otherwise consistent |
| https://www.solomon-systech.com/?p=5620 | Solomon product page; no PDF link in static HTML; not used |
| https://www.good-display.com/companyfile/620.html | Product-page entry for the GDEY0154D67 specification (download button reveals the PDF URL below) |
| https://v4.cecdn.yun300.cn/100001_1909185148/GDEY0154D67-new.pdf | Used. Good Display GDEY0154D67 specification **Rev 1.2, 2024-04-17** (38 pp.; needs a Referer header); pin table, update times, sequence figure, temperature limits |
| https://www.good-display.com/news/80.html | Used. GD usage guidelines: 180 s minimum update interval, full refresh after 5 partials, 24 h refresh (colour), storage |

## Bosch BMA423 and SensorAPI
| URL | Note |
|---|---|
| https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/876/BMA423_Rev2.0_Aug2019.pdf | Used. Bosch BMA423 datasheet **rev 2.0, Aug 2019** (101 pp.): init, power modes, current table, INT registers, I2C timing, self-test |
| https://github.com/boschsensortec/BMA423_SensorAPI | **404.** The repo named in the brief does not exist; same for .../BMA423-Sensor-API and .../BMA4_SensorAPI |
| https://api.github.com/orgs/boschsensortec/repos | Used. Org listing (33 repos): BMA400/456/530/580 APIs, no BMA423 |
| https://github.com/boschsensortec/BMA456_SensorAPI (+ git trees/tags/commits via api.github.com) | Used. Active Bosch BMA4 repo: v2.29.0, commit 3266db2c5de1, 2024-02-05, BSD-3-Clause; common bma4.* core, no BMA423 files |
| https://github.com/sqfmi/BMA423-Sensor-API (tag bma423_v2.14.13, commit df5c8ee95f7544451090f70eec4911fb6c9a7c72; raw files under https://raw.githubusercontent.com/sqfmi/BMA423-Sensor-API/bma423_v2.14.13/) | Used. Surviving copy of Bosch's BMA423 SensorAPI (commits by "Bosch Sensortec"): LICENSE text, bma423.h/bma4.h/bma4_defs.h API names and defines, bma423.c blob byte count and hash |
| https://github.com/Deadroad/BMA423-Sensor-API | Used for metadata only: older mirror (2018, v1.1.4), shows the lineage |
| https://github.com/boschsensortec/COINES_SDK (tree listing) | Checked for a BMA423 copy: none |
| https://www.bosch-sensortec.com/products/motion-sensors/accelerometers/ and .../bma423/ | Accelerometer overview lists BMA400/BMA422/BMA456 (BMA4xx); the BMA423 product page is 404 |
| https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bma456-ds000.pdf | Fetched to learn Bosch's current datasheet naming (BMA456 rev 3.6, Mar 2026); `bst-bma423-ds000.pdf` is 404 |
| https://www.bosch-sensortec.com/news/bosch-announces-high-performance-mems-acceleration-sensors-for-wearables.html | Step counter "< 30 uA" statement, seen in a search-result summary only (page not fetched) |
| https://www.revk.uk/2023/07/bma423.html | Third-party blog saying rev 2.0 is current and rev 1.1 copies are "incorrect"; search-result summary only, unverified |
| web.archive.org (Wayback) | WebFetch blocked; the availability API answered HTTP 429; the original official BMA423 repo could not be recovered |
