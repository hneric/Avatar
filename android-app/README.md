# S31 Companion Android App

Android companion app for ESP32-S31.

## Current Scope

- Scan BLE devices named `S31_xxxxxx`.
- Provision device Wi-Fi through Espressif BLE provisioning SDK.
- Bind one device by generating an app token and writing:

```json
{"event":"bind","token":"<token>"}
```

- Detect nearby bound device by RSSI and write:

```json
{"event":"nearby","token":"<token>"}
```

- Include Espressif provisioning SDK dependency for BLE Wi-Fi provisioning.
- Reset binding by writing:

```json
{"event":"reset_bind","token":"<token>"}
```

## Device Protocol

Presence service:

```text
Service UUID:        53455250-3153-9991-6a44-b27d00005331
Characteristic UUID: 53455250-3153-9991-6a44-b27d01005331
```

Device name:

```text
S31_xxxxxx
```

## Product Flow

First use when device is not provisioned:

```text
Open app -> scan S31 -> provision Wi-Fi -> bind token -> save device locally
```

Daily use:

```text
App scans -> known S31 RSSI is close enough -> write nearby with token -> device greets -> BLE disconnects
```

## Notes

- RSSI threshold is currently `-65 dBm`.
- Nearby cooldown is currently `30 seconds` in the app.
- Firmware also has a Presence cooldown.
- Wi-Fi provisioning uses PoP `s31pop`.
- If the device already has Wi-Fi credentials, erase Wi-Fi/NVS on the device before testing provisioning.
