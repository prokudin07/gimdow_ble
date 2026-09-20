# ESPHome Gimdow BLE

External ESPHome component for direct local BLE control of a **Gimdow A1 PRO MAX** smart lock using the Tuya BLE v3 protocol.

Tested with:
- Gimdow A1 PRO MAX / Tuya product profile `rlyxv7pe`
- ESPHome 2026.9.0
- ESP32 using `ble_client`

## Features

- Direct local BLE lock/unlock
- Tuya BLE v3 session setup and pairing
- Lock command via DP46
- Unlock command via DP6
- Lock-state fallback from DP47
- Optional external binary sensor as the authoritative physical bolt state
- Compatible with `ble_client.auto_connect: false` so a separate physical BLE remote can still access the lock

## Installation

```yaml
external_components:
  - source: github://prokudin07/gimdow_ble
    components: [ gimdow_ble ]
    refresh: 0s
```

## Basic example

```yaml
substitutions:
  gimdow_local_key: "YOUR_LOCAL_KEY"
  gimdow_mac: "AA:BB:CC:DD:EE:FF"

esp32_ble_tracker:

ble_client:
  - mac_address: ${gimdow_mac}
    id: gimdow_ble_client
    auto_connect: false

external_components:
  - source: github://prokudin07/gimdow_ble
    components: [ gimdow_ble ]
    refresh: 0s

lock:
  - platform: gimdow_ble
    name: Gimdow
    id: gimdow

    ble_client_id: gimdow_ble_client

    local_key: ${gimdow_local_key}
    uuid: "YOUR_TUYA_UUID"
    tuya_device_id: "YOUR_TUYA_DEVICE_ID"
```

## Optional physical bolt-state sensor

If the lock can also be operated by another BLE remote, the Tuya DP state may not always be updated through this ESP32 connection. You can therefore provide an existing ESPHome binary sensor as the authoritative lock state:

```yaml
lock:
  - platform: gimdow_ble
    name: Gimdow
    id: gimdow

    ble_client_id: gimdow_ble_client
    local_key: ${gimdow_local_key}
    uuid: "YOUR_TUYA_UUID"
    tuya_device_id: "YOUR_TUYA_DEVICE_ID"

    state_sensor: Lock_sensor
```

For the current implementation:
- external sensor `ON` = unlocked
- external sensor `OFF` = locked

When `state_sensor` is configured, DP47 and optimistic command-state publishing do not overwrite the external physical state.

## BLE connection mode

If you also use the original physical BLE remote, use:

```yaml
auto_connect: false
```

The component will connect on demand when Home Assistant sends a lock/unlock command.

If the ESP32 is the only BLE controller and minimum latency matters, `auto_connect: true` can be used, but it may prevent another BLE remote from connecting while the ESP32 holds the connection.

## Required Tuya values

The component requires:
- BLE MAC address
- Tuya `local_key`
- Tuya UUID
- Tuya device ID

Do **not** commit your real `local_key` to a public repository. Prefer ESPHome `secrets.yaml` or substitutions sourced from secrets.

## Status

This is an experimental external component developed and tested against a specific Gimdow A1 PRO MAX / `rlyxv7pe` lock. Other Tuya BLE locks may use different datapoints or protocol behavior.
