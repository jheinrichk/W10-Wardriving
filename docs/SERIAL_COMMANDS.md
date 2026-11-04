# USB serial command reference

The sketch starts USB serial at 115200 baud. Command input is line based. The device emits newline delimited JSON (NDJSON) and diagnostic text. Send `HELP` to see the complete on device command list.

## General and diagnostics

| Command | Purpose |
|---|---|
| `HELP` | Print help |
| `STATUS` | Emit current status |
| `SCAN QA` | Scan health and packet-processing counters |
| `SCAN TOP` | Top aggregate public OUI, company ID, UUID and name observations |
| `SCAN TOP RESET` | Reset aggregate survey counters |
| `WATCH SELFTEST` | Run built in matching tests using synthetic input |

## Detection selection and UI: initial v0.4.20 release

| Command | Purpose |
|---|---|
| `SELECT` / `SELECT LIST` | Print all eleven categories, their modes and flash states |
| `SELECT FLOCK SHOW` | Display and alert for FLOCK without logging the hits |
| `SELECT TRACKER REC` | Log TRACKER detections without displaying or sounding alerts |
| `SELECT ALL ON` | Set all categories to `SHOW+REC` (`ON` is a synonym) |
| `SELECT <category> OFF` | Ignore matches in the selected category before device tracking |
| `SELECT FLASH RAVEN ON` | Enable the two second Raven color flash |
| `SELECT FLASH <category\|ALL> ON\|OFF` | Control flash state, saved in NVS |
| `HITS CLEAR` | Clear count, bar, device table and the current startup session log |
| `MENU` | Open or close the non-touch W10 display selection menu |

Category modes cycle `SHOW+REC → SHOW → REC → OFF` from the display or browser. The current source uses a 60 second minimum interval between reports for the same tracked device. `hits` is a report count; `packets` is a raw packet count. The eleven categories are AXON, FLOCK, RAVEN, RING, DRONE, TRACKER, GLASSES, FLIPPER, RADIO, LORA and OTHER.

See [UI guide](USER_INTERFACE.md) for W10 two-button controls, flash and ticker behavior. The browser screenshots show a developmental nine category interface; the initial public release has eleven categories.

## SX1262 RADIO and LORA receive monitoring

| Command | Purpose |
|---|---|
| `SUBG` / `SUBG STATUS` | Show SX1262 presence, scanner state, sweep count, LoRa profile list and recent observations |
| `SUBG SCAN` | Print last RF sweep, RSSI and learned noise-floor samples |
| `SUBG ON` / `SUBG OFF` | Enable/disable receive scanning (saved) |
| `SUBG THRESHOLD -70` | Set minimum RF energy reporting threshold in dBm (allowed `-120..-30`, saved) |
| `SUBG BAND ADD 450 470 UHF` | Add a named RF sweep band (150–960 MHz, up to six bands) |
| `SUBG BAND CLEAR` | Restore default 700/800 MHz sweep bands |
| `SUBG LORA ADD 915.0 125 10 5 34 2 Example` | Add LoRa receive profile: MHz, kHz BW, SF, coding-rate denominator, hexadecimal sync, kind (0 generic, 1 Meshtastic, 2 LoRaWAN), optional label |
| `SUBG LORA CLEAR` | Restore default Meshtastic and US915 LoRaWAN receive profiles |
| `SUBG RESET` | Reinitialize SX1262 radio |
| `SELECT RADIO SHOW` | Display RADIO detections without logging |
| `SELECT LORA REC` | Log LoRa observations without display or sound |
| `SELECT FLASH RADIO ON` | Enable flashing on qualifying RF-activity reports |

Observation only: No voice or encrypted payload decoding and no automatic RadioReference agency lookup. Refer to [the SX1262 radio guide](SX1262_RADIO_LORA.md) for frequencies, identification limitations and event fields.

## Watchlist and alias management

| Command | Purpose |
|---|---|
| `WATCH LIST` / `WATCH EXPORT` | Display active rules |
| `WATCH ADD TYPE ID \| Description` | Add a custom watch rule (use actual `|` separator) |
| `WATCH REMOVE <number>` | Remove rule by displayed index |
| `WATCH CLEAR YES` | Clear current rules |
| `WATCH SAVE` | Persist current rules |
| `WATCH REPO MERGE` | Merge all 64 built in rules |
| `WATCH REPO REPLACE` | Replace current rules with 64 built ins |
| `WATCH FLOCK BLE MERGE` | Merge 4 Flock related entries |
| `WATCH AXON BLE MERGE` | Merge FE6B and FE6C |
| `WATCH PRESET AXON` / `WATCH PRESET META` | Add a named preset |
| `WATCH VALIDATORS ON` / `WATCH VALIDATORS OFF` | Toggle optional advertisement validators |
| `WATCH IMPORT BEGIN` / `WATCH IMPORT REPLACE` | Begin staged merge or replacement import |
| `WATCH IMPORT COMMIT` / `WATCH IMPORT CANCEL` | Finalize or cancel an import |
| `ALIAS LIST` | List saved device aliases |
| `ALIAS SET MAC \| Nickname` | Associate a nickname with a MAC address |
| `ALIAS REMOVE MAC` | Remove an alias |

## Logging

| Command | Purpose |
|---|---|
| `LOG ON` | Start/rearm flash logging |
| `LOG OFF` | Stop flash logging while keeping USB output |
| `LOG LIST` | List log files stored in LittleFS |
| `LOG DUMP /logs/<name>` | Send one stored log over serial |
| `FORMATFS YES` | Erase and format the LittleFS filesystem |

## Wi-Fi and browser mapping

| Command | Purpose |
|---|---|
| `WIFI STATUS` | Show Wi-Fi operating status |
| `WIFI ON` / `WIFI OFF` | Enable or disable Wi-Fi |
| `WIFI SET <hotspot ssid> \| <password>` | Configure station mode on a phone hotspot |
| `WIFI AP <ssid> \| <password>` | Configure the device's access point |
| `WIFI CLEAR` | Clear hotspot credentials and return to AP mode |

When joined to a hotspot, the web map is available at the displayed IP address or `http://w10.local` when mDNS resolves. The page is served from the W10 and may need internet access for background map tiles.

## Alerts and power

| Command | Purpose |
|---|---|
| `SOUND ON` / `SOUND OFF` | Toggle alert playback |
| `SOUND VOLUME 0..85` | Change persistent codec volume |
| `SOUND TEST NEW` | Test a new-match tone |
| `SOUND TEST REPEAT` | Test a reacquisition tone |
| `SOUND TEST PRIORITY` | Test a priority tone |
| `SOUND TEST SUSTAINED` | Test a sustained-match tone |
| `SOUND TEST READY` | Test the ready tone |
| `LED CHARGE OFF` / `LED CHARGE ON` / `LED CHARGE DISABLE` | Control the AXP2101 charger LED mode |

## Web endpoints

The firmware exposes the dashboard (`GET /`) and read APIs (`GET /api/status`, `/api/sessions`, `/api/tail`, `/api/select`). `GET /api/select?cat=...&mode=...` also changes persisted category selections, `GET /api/select?cat=...&flash=...` changes flash preferences, and `GET /api/clear?yes=1` clears live hits and the active session log. These routes are not authenticated; anyone who can reach the web server may be able to read or alter this device state.

