# Security and personal privacy review: v0.4.20

Scope: Static review of the supplied `W10_OUI_DETECTION_0_4_20.ino`, plus repository screenshots. This document describes the delivered source, not an independently conducted penetration test or verified remediation. No firmware modifications were made while preparing the GitHub files.

## High-priority concerns

1. Unauthenticated GPS log read access (high): `/api/status`, `/api/sessions` and `/api/tail` are registered without authentication (source lines 2803–2805). Logs may include geotagged observations, BLE identifiers and route timestamps (lines 1447–1488 and 1644–1667). Anyone who joins the W10 AP or can reach the web server in station mode can access them, subject to network isolation. Personal consequence: Exposure of driving routes and visited locations. Add access control and minimize logged personal data.
2. Unauthenticated web changes and destructive CLEAR action (high): `/api/select` and `/api/clear` use HTTP GET without login (lines 2739–2763 and 2806–2807). Requests can change saved detection/flash behavior in NVS or clear the active session log. `GET /api/clear?yes=1` triggers clearing without a second device side confirmation. A party with access to the network can alter how detections are displayed or recorded or erase the current session. These actions also have cross site request forgery concerns because they mutate state via GET without tokens. Require authentication, CSRF defenses and POST for state changing endpoints.
3. Shared/default Wi-Fi password (high): `W10-OUI` and `w10detect` are the hardcoded defaults (source lines 403–404). The firmware also prints AP credentials to USB serial (Wi-Fi serial diagnostics). Nearby people who know the defaults can join a device that has not changed them. Change the live unit's AP password with `WIFI AP W10-OUI | <unique-password>` over serial, and avoid printing credentials by default.
4. Possible HTML injection in map popups (high if an attacker can control a stored label): JavaScript interpolates `p.lab`, `p.ident` and related fields into Leaflet popup HTML without escaping (source lines 2582 and 2623). If attacker-controlled aliases/rule names reach these strings, they could execute script in the dashboard origin. This is a potential attack path rather than a confirmed over the air exploit. Use DOM text nodes or escape all untrusted strings before HTML insertion.

## Other security and privacy concerns

5. HTTP without TLS: `WebServer(80)` (line 407). Treat the AP or LAN as the security boundary. Do not expose the page on an untrusted shared network or via internet port forwarding.
6. Third-party dashboard resources: The browser loads Leaflet from `unpkg.com` and map tiles from `tile.openstreetmap.org` (lines 2533, 2569–2570 and 2620). Providers may learn map viewport requests, browser IP and related metadata. The page also executes externally hosted JavaScript. Consider bundled pinned scripts and offline map options.
7. GPS and BLE data retention: Device identifiers, GPS points, UTC time and raw advertising hex are sent as serial NDJSON and can be saved to LittleFS (lines 1447–1488 and 1644–1667). Exports may reveal routes or co-observed devices. Use limited retention, redact before sharing, and avoid publishing captures.
8. Plain NVS credential storage: AP and hotspot credentials are saved through `Preferences` (around lines 2669–2675). The source does not configure ESP32 flash encryption. Physical device access may expose those credentials.
9. Filesystem auto-format risk: An unsuccessful LittleFS mount can lead to formatting under the boot recovery path (lines 1013–1017). This may destroy saved records. Make formatting an explicit owner-approved recovery operation rather than an automatic action.

## SX1262 location and identity metadata

RADIO detections may expose the user's geotagged observations of RF activity. LORA data can include persistent Meshtastic node identifiers and LoRaWAN DevEUI/DevAddr values. The logs are stored locally and exposed over the unauthenticated web interface to any connected client; avoid publishing raw exported sessions. The SX1262 code is passive receive only, but the latest radio paths have not been independently exercised on hardware in this packaging review.

## Photo metadata and visual contents

All eight GitHub-ready images have been reencoded without identifying EXIF/PNG ancillary metadata. The original supplied hardware JPG had geolocation metadata; the repository copy does not. However, the map screenshots still visibly show locations and detection information. Removing metadata does not anonymize the visible pixels.

## Positive findings

- The supplied WebServer route table contains no firmware upload or shell/command-execution endpoint (lines 2294–2300). It does contain remote state changing endpoints; do not characterize the web API as read-only.
- `/api/tail` limits names to `.jsonl` and rejects `/` and `..` (the `webSendTail` route), reducing straightforward traversal attacks.
- The SX1262 scan code is configured for reception and contains no LoRa transmit command sequence. It can store linkable Meshtastic node IDs, LoRaWAN DevEUI/DevAddr values, RF frequencies and GPS coordinates in addition to BLE data.

## Publication note

Publishing the code itself does not transmit an owner's existing device logs or hotspot password. The larger personal exposure comes from posting unredacted field screenshots, sharing raw captures and operating a W10 with its default AP password or an untrusted network. A public repository's default Wi-Fi password is widely visible, so do not rely on it as a secret on your own unit.
