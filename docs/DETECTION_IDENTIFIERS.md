# Detection identifier reference

These identifiers are present in the unmodified v0.4.20 sketch. These are the built in signature tables for the initial release. It documents identifiers in firmware source, not independent verification that they uniquely identify a particular product or device.

Interpretation: `Possible` means a candidate signature. Manufacturer identifiers, service UUIDs and BLE names are not device proof. Advertising addresses may be randomized or spoofed. A matching signal does not verify a nearby physical device is present or functioning as described. The firmware assigns LOW, MED or HIGH tiers; these are internal matching priorities, not calibrated probabilities.

Types: `OUI` = public MAC address prefix; `MAC` = full device address (supported for custom entries); `CID` = manufacturer/company ID; `SVC` = 16-bit service UUID; `NAME` = advertising name pattern; `META` = composite signature; `UUID128` = 128-bit UUID.

## Core device rules (30)

| Type | Identifier | Description in firmware | Tier |
|---|---|---|---|
| `OUI` | `187F88` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `242BD6` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `343EA4` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `54E019` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `5C475E` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `649A63` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `90486C` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `9C7613` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `AC9FC3` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `C4DBAD` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `CC3BFB` | Possible Ring doorbell, security camera or chime | Med |
| `OUI` | `0025DF` | Possible Axon body camera or fleet device (IEEE OUI) | High |
| `CID` | `034D` | Possible Axon body camera or TASER (manufacturer ID) | High |
| `SVC` | `FC81` | Possible Axon device (advertised service UUID) | High |
| `META` | `META` | Possible Ray-Ban or Oakley Meta smart glasses (composite) | High |
| `OUI` | `B41E52` | Possible Flock Safety ALPR camera (IEEE OUI) | Med |
| `OUI` | `0C9AE6` | Possible DJI drone or related device | Med |
| `OUI` | `8C5823` | Possible DJI drone or related device | Med |
| `OUI` | `04A85A` | Possible DJI drone or related device | Med |
| `OUI` | `58B858` | Possible DJI drone or related device | Med |
| `OUI` | `E47A2C` | Possible DJI drone or related device | Med |
| `OUI` | `60601F` | Possible DJI drone or related device | Med |
| `OUI` | `481CB9` | Possible DJI drone or related device | Med |
| `OUI` | `34D262` | Possible DJI drone or related device | Med |
| `OUI` | `00121C` | Possible Parrot drone or related device | Med |
| `OUI` | `00267E` | Possible Parrot drone or related device | Med |
| `OUI` | `9003B7` | Possible Parrot drone or related device | Med |
| `OUI` | `903AE6` | Possible Parrot drone or related device | Med |
| `OUI` | `A0143D` | Possible Parrot drone or related device | Med |
| `OUI` | `381D14` | Possible Skydio drone or related device | Med |

## Flock related additions (4)

| Type | Identifier | Description in firmware | Tier |
|---|---|---|---|
| `CID` | `09C8` | XUNTONG manufacturer ID (NOT proof of Flock hardware) | Low |
| `NAME` | `Penguin-` | Possible Flock Penguin battery | Med |
| `NAME` | `FS Ext Battery` | Possible Flock external battery | Med |
| `UUID128` | `E8CCBB38953246A89FE51814DF172E6F` | Possible Flock accessory GATT service | Med |

## Axon service additions (2)

| Type | Identifier | Description in firmware | Tier |
|---|---|---|---|
| `SVC` | `FE6B` | Possible Axon or TASER device (FE6B service UUID) | High |
| `SVC` | `FE6C` | Possible Axon or TASER device (FE6C service UUID) | High |

## Additional BLE signatures (28)

| Type | Identifier | Description in firmware | Tier |
|---|---|---|---|
| `SVC` | `FD5A` | Possible Samsung SmartTag | Med |
| `SVC` | `FEED` | Possible Tile tracker | Med |
| `SVC` | `FEEC` | Possible Tile tracker | Med |
| `SVC` | `FD84` | Possible Tile tracker | Med |
| `SVC` | `FCB2` | DULT compatible tracker signal | Med |
| `SVC` | `FE33` | Possible Chipolo tracker | Med |
| `SVC` | `FE65` | Possible Chipolo tracker | Med |
| `SVC` | `FD5F` | Possible Ray-Ban Meta service | Med |
| `SVC` | `FEB7` | Possible Meta radio service | Med |
| `SVC` | `FEB8` | Possible Meta radio service | Med |
| `SVC` | `FE45` | Possible Snap radio service | Med |
| `SVC` | `3081` | Possible Flipper Zero service | High |
| `SVC` | `3082` | Possible Flipper Zero service | High |
| `SVC` | `3083` | Possible Flipper Zero service | High |
| `SVC` | `FFFA` | Drone Remote ID broadcast (ASTM F3411) | Med |
| `SVC` | `3100` | Possible Raven gunshot sensor service | High |
| `SVC` | `3200` | Possible Raven gunshot sensor service | High |
| `SVC` | `3300` | Possible Raven gunshot sensor service | High |
| `SVC` | `3400` | Possible Raven gunshot sensor service | High |
| `SVC` | `3500` | Possible Raven gunshot sensor service | High |
| `NAME` | `Pigvision` | Possible Flock accessory by name | Med |
| `NAME` | `Flock_Setup` | Possible Flock setup mode by name | High |
| `NAME` | `BodyWorn` | Possible body camera by name | Med |
| `NAME` | `Axon` | Possible Axon device by name | Med |
| `CID` | `060C` | Vuzix Bluetooth manufacturer (brand only) | Low |
| `CID` | `03C2` | Snap Bluetooth manufacturer (brand only) | Low |
| `CID` | `01AB` | Meta Bluetooth manufacturer (brand only, not glasses) | Low |
| `CID` | `08C3` | Chipolo manufacturer ID (brand only) | Low |

## Other logic beyond the 64 listed rules

The sketch also implements built in advertisement payload analysis, including a BWCDEVICE tag, FS-hex device names, ten-digit device names, Apple Find My, Google Find My Device network and Drone Remote ID data. These are not additional entries in the 64-entry static rule inventory. See the firmware's `runMatcherSelfTest()` implementation for test cases.

Important example: `CID 09C8` is labeled XUNTONG and is expressly not proof of Flock hardware. Likewise an `OUI` does not by itself establish that a particular camera, drone or wearable has been detected.

## Rule management

- `WATCH LIST` or `WATCH EXPORT` displays the active rule list.
- `WATCH REPO MERGE` adds missing built in rules without replacing user entries.
- `WATCH REPO REPLACE` replaces the current list with the 64 built in rules.
- `WATCH FLOCK BLE MERGE` and `WATCH AXON BLE MERGE` add their respective subsets.
- `WATCH VALIDATORS ON` or `OFF` toggles optional advertisement validators.

Rule capacity in firmware is 160 entries.
