# SX1262 RADIO + LORA receive monitoring: v0.4.20

The initial public release uses the Meshnology W10 SX1262 as a receive-only sub-GHz monitor, multiplexed between RF-energy sampling and LoRa channel-activity detection (CAD). The device still performs BLE scanning and GPS-tagged logging through the ESP32-S3.

## RADIO: public-safety-band RF activity

| Parameter | Firmware default |
|---|---|
| Band A | 799.5 to 805.5 MHz (`700 mobile`) |
| Band B | 806.0 to 813.5 MHz (`800 mobile`) |
| Sampling step | 0.4 MHz |
| Minimum RSSI | −70 dBm |
| Noise-floor margin | 20 dB above the learned floor for the step |
| Repeat limit | 60 seconds per frequency identifier |
| Color | Violet |
| Display flash | Enabled by default |
| Sound | Priority alert when SHOW is enabled |
| Output | `subg` JSON event with frequency, RSSI, floor, source band, GPS (when fixed) |

A sample label is `Radio keyed 810.2 MHz`. This is a convenience label for RF energy, not proof that a specific radio was keyed. Measurement alone does not identify the transmitter, distinguish a police officer from another public-safety, transit or public-works user, or establish distance to the transmitter. These are intended mobile/control transmit ranges of paired U.S. public-safety and land-mobile systems (see [47 CFR § 90.531](https://www.law.cornell.edu/cfr/text/47/90.531) and [47 CFR § 90.613](https://www.law.cornell.edu/cfr/text/47/90.613)), but signals may arise from other authorized users, interference or unexpected emitters. The device does not decode P25 conversations.

For external band/frequency research, consult [RadioReference](https://www.radioreference.com/). This source is not integrated with the firmware and no automatic agency lookup is implemented.

RF sampling is interleaved with LoRa CAD and BLE processing. The intended rate is about ten sweeps per second, but the code does not enforce a fixed sweep frequency; inspect live `SUBG` sweep counters for actual performance. `SUBG SCAN` lists the last RSSI/learned floor for each step; adjust `SUBG THRESHOLD` based on observed local RF conditions.

## LORA: Meshtastic and LoRaWAN

| Profile | Frequency / setting | On-air information used |
|---|---|---|
| Meshtastic US LongFast | 906.875 MHz, BW 250 kHz, SF11, CR 4/5, sync `0x2B` | Clear frame header's origin node ID and routing fields when received |
| LoRaWAN US915 sub-band 2 uplinks | 903.9 to 905.3 MHz at 0.2 MHz spacing, BW 125 kHz, SF10 | Join-request DevEUI / JoinEUI, or data-frame DevAddr when decoded |
| LoRaWAN 500 kHz uplink | 904.6 MHz, BW 500 kHz, SF8 | Same LoRaWAN header fields where present |

These are profiles programmed in the sketch, not an all-frequency scanner. `SUBG LORA ADD` adds a compatible custom profile. The existing implementation reads up to 64 bytes from received payloads and does not decrypt protected Meshtastic or LoRaWAN application data.

Received LoRa frames generate low-confidence LORA events. They may be shown dimly and recorded in USB/LittleFS logs or on the map when REC is enabled, without sound or screen flash. Decoded DevEUI, DevAddr and Meshtastic node IDs may be persistent or linkable identifiers, so treat any GPS-tagged export as potentially sensitive.

## Menu, website and serial

The categories are AXON, FLOCK, RAVEN, RING, DRONE, TRACKER, GLASSES, FLIPPER, RADIO, LORA, OTHER. Both new categories default to SHOW+REC; RADIO has flashing enabled by default and LORA does not. The existing BOOT and PWR button menu, USB `SELECT` command and browser Select panel configure both. Mode and flash selections are saved to ESP32 NVS.

```text
SUBG                     # report chip state, active phase, counters and profiles
SUBG SCAN                # display latest per-step RSSI / noise floor
SUBG ON                  # enable RF and LoRa receive monitoring
SUBG OFF                 # disable the sub-GHz subsystem
SUBG THRESHOLD -70       # set RF energy threshold (dBm)
SUBG BAND ADD 450 470 UHF
SUBG BAND CLEAR         # restore default bands
SUBG LORA ADD 915.0 125 10 5 34 2 Example
SUBG LORA CLEAR         # restore default LoRa profiles
SUBG RESET              # reinitialize chip
SELECT RADIO SHOW+REC
SELECT LORA REC
SELECT FLASH RADIO ON
STATUS                   # includes subg state
```

All radio observations are passive. The sketch implements no SX1262 transmission workflow. The code was statically inspected for this documentation; the radio path has not been compiled or measured on W10 hardware as part of repository preparation.
