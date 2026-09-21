# ESPHome Gimdow BLE

External ESPHome component for direct local BLE control of supported Gimdow/Tuya smart locks.

## Supported lock profiles

| `model` | Tested/product profile | Protocol |
|---|---|---|
| `a1_pro_max` | Gimdow A1 PRO MAX / `rlyxv7pe` | Tuya BLE v3, service `0x1910` |
| `a1_ultra` | Gimdow/Raykube A1 Ultra / `hc7n0urm` | TuyaOS FD50 / V4 |

The `model` option is **required** so a firmware update cannot silently switch a lock to a different protocol implementation.

The A1 PRO MAX profile has been tested directly with this ESPHome component. The A1 Ultra profile is based on physically verified FD50/V4 lock and unlock traffic from the Tuya-BLE project, but should still be considered experimental in this ESPHome port until tested on real Gimdow Ultra hardware.

## Versioning

For installed/production devices, pin the component to a version ref instead of following `main`.

- `v1.0.0` — original A1 PRO MAX-only component; no `model` option.
- `v2.0.0` — required `model` option and support for A1 PRO MAX + A1 Ultra.
- `v2.1.0` — adds battery-state diagnostics from DP9.
- `main` — development branch and may contain breaking changes.

ESPHome supports a branch or tag after `@` in a GitHub external-component source.

Recommended:

```yaml
external_components:
  - source: github://prokudin07/gimdow_ble@v2.1.0
    components: [ gimdow_ble ]
```

If an existing device is still using the old configuration and you do not want to migrate it yet:

```yaml
external_components:
  - source: github://prokudin07/gimdow_ble@v1.0.0
    components: [ gimdow_ble ]
```

Do not use `refresh: 0s` with `main` on a production lock unless you deliberately want every new repository change to be pulled into the next compile.

## A1 PRO MAX example

```yaml
substitutions:
  # Replace these values with the ones from your own lock.
  gimdow_local_key: "YOUR_LOCAL_KEY"
  gimdow_mac: "AA:BB:CC:DD:EE:FF"
  gimdow_uuid: "YOUR_TUYA_UUID"
  gimdow_device_id: "YOUR_TUYA_DEVICE_ID"

esp32_ble_tracker:

ble_client:
  - mac_address: ${gimdow_mac}
    id: gimdow_ble_client
    auto_connect: false

external_components:
  - source: github://prokudin07/gimdow_ble@v2.1.0
    components: [ gimdow_ble ]

lock:
  - platform: gimdow_ble
    name: Gimdow
    id: gimdow

    model: a1_pro_max

    ble_client_id: gimdow_ble_client

    local_key: ${gimdow_local_key}
    uuid: ${gimdow_uuid}
    tuya_device_id: ${gimdow_device_id}

    # Optional authoritative physical bolt sensor.
    # ON = UNLOCKED, OFF = LOCKED.
    # state_sensor: Lock_sensor
```

### A1 PRO MAX protocol

- Service: `0x1910`
- Notify: `0x2B10`
- Write: `0x2B11`
- Unlock: DP6 = true
- Lock: DP46 = true
- State fallback: DP47, true = unlocked, false = locked

## A1 Ultra example

A1 Ultra requires one additional device-specific value, `ble_unlock_check`, for remote unlock.

```yaml
substitutions:
  # Replace these values with the ones from your own lock.
  gimdow_local_key: "YOUR_LOCAL_KEY"
  gimdow_mac: "AA:BB:CC:DD:EE:FF"
  gimdow_uuid: "YOUR_TUYA_UUID"
  gimdow_device_id: "YOUR_TUYA_DEVICE_ID"
  gimdow_ble_unlock_check: "YOUR_BLE_UNLOCK_CHECK"

esp32_ble_tracker:

ble_client:
  - mac_address: ${gimdow_mac}
    id: gimdow_ble_client
    auto_connect: false

external_components:
  - source: github://prokudin07/gimdow_ble@v2.1.0
    components: [ gimdow_ble ]

lock:
  - platform: gimdow_ble
    name: Gimdow Ultra
    id: gimdow_ultra

    model: a1_ultra

    ble_client_id: gimdow_ble_client

    local_key: ${gimdow_local_key}
    uuid: ${gimdow_uuid}
    tuya_device_id: ${gimdow_device_id}
    ble_unlock_check: ${gimdow_ble_unlock_check}

    # Optional authoritative physical bolt sensor.
    # ON = UNLOCKED, OFF = LOCKED.
    # state_sensor: Lock_sensor
```

If `model: a1_ultra` is selected without `ble_unlock_check`, ESPHome configuration validation fails intentionally.

### A1 Ultra protocol

- Product profile observed: `hc7n0urm`
- Service: `0000fd50-0000-1000-8000-00805f9b34fb`
- Write: `00000001-0000-1001-8001-00805f9b07d0`
- Notify: `00000002-0000-1001-8001-00805f9b07d0`
- DEVICE_INFO payload: `00 f3`
- DEVICE_INFO packet marker: `0x20`
- DEVICE_INFO is sent in one larger ATT write after MTU negotiation
- V4 command: `FUN_SENDER_DPS_V4 = 0x0027`
- Lock: `manual_lock` / DP46 in V4 framing
- Unlock: V4 payload built from the device-specific `ble_unlock_check`

## Required Tuya values

The common parameters are:

| ESPHome option | What it is | Where to get it |
|---|---|---|
| `mac_address` / `gimdow_mac` | BLE MAC address | Tuya Smart / Smart Life device information |
| `tuya_device_id` | Tuya device ID | Shown as **Virtual ID** in the app |
| `uuid` | Tuya BLE UUID | Tuya Cloud/OpenAPI device information |
| `local_key` | Local Tuya encryption key | Tuya Cloud/OpenAPI or TinyTuya |
| `ble_unlock_check` | A1 Ultra unlock check payload | Tuya Cloud/OpenAPI device status, code `ble_unlock_check` |

### MAC and Virtual ID from the mobile app

Open the lock in **Tuya Smart / Smart Life** and open its device information page.

Look for:

- **MAC** — use as the ESPHome BLE address.
- **Virtual ID** — use as `tuya_device_id`.

![Tuya Smart / Smart Life device information showing Virtual ID and MAC](images/tuya-device-info.svg)

Example:

```text
Virtual ID: bf59ffwdyww949lh
MAC:        DC:23:4E:D1:FC:AD
```

These are examples only. Use the values from your own lock.

### UUID and local_key

Link the Tuya Smart / Smart Life account to a Tuya IoT Cloud project, find the device by its Virtual ID / Device ID, and read the device information through Tuya Cloud/OpenAPI.

TinyTuya can also retrieve the common Tuya device credentials:

```bash
python3 -m venv venv
source venv/bin/activate
pip install tinytuya
python -m tinytuya wizard
```

### A1 Ultra: ble_unlock_check

For A1 Ultra, query the device status through Tuya IoT OpenAPI and find the status item:

```json
{
  "code": "ble_unlock_check",
  "value": "BASE64_DEVICE_SPECIFIC_VALUE"
}
```

Use the complete raw base64 string from `value`:

```yaml
gimdow_ble_unlock_check: "BASE64_DEVICE_SPECIFIC_VALUE"
```

This value is device-specific. Without it, the A1 Ultra remote-unlock payload cannot be built.

## Secrets

Do not publish a real `local_key` or `ble_unlock_check` in a public repository.

Recommended:

```yaml
# secrets.yaml
gimdow_local_key: "YOUR_REAL_LOCAL_KEY"
gimdow_ble_unlock_check: "YOUR_REAL_BLE_UNLOCK_CHECK"
```

Then reference those values from the device configuration.

## Lock configuration selects

Both supported profiles expose the Tuya DP31 beep-volume configuration datapoint:

- DP31 — beep volume: `mute`, `low`, `normal`, `high`

Enable them as optional ESPHome select entities:

```yaml
lock:
  - platform: gimdow_ble
    name: Gimdow
    id: gimdow

    model: a1_pro_max
    ble_client_id: gimdow_ble_client
    local_key: ${gimdow_local_key}
    uuid: ${gimdow_uuid}
    tuya_device_id: ${gimdow_device_id}

    beep_volume:
      name: "Gimdow Beep Volume"
```

For A1 PRO MAX this is sent as a Tuya BLE v3 enum write. For A1 Ultra it uses the FD50/V4 command framing.

The last known beep-volume value is stored in ESPHome preferences and restored immediately after an ESP reboot. The component also performs one BLE connection shortly after startup so DP31 can be refreshed from the lock when the lock reports its current configuration.

## Battery diagnostics

Both supported jtmspro lock profiles expose Tuya DP9 as a battery-state enum.

The component can publish four optional diagnostic entities:

```yaml
lock:
  - platform: gimdow_ble
    name: Gimdow
    id: gimdow

    model: a1_pro_max
    ble_client_id: gimdow_ble_client
    local_key: ${gimdow_local_key}
    uuid: ${gimdow_uuid}
    tuya_device_id: ${gimdow_device_id}

    battery_state:
      name: "Gimdow Battery State"

    battery_state_code:
      name: "Gimdow Battery State Code"

    battery_low:
      name: "Gimdow Battery Low"

    battery_critical:
      name: "Gimdow Battery Code 3"
```

Known DP9 mapping from the Tuya-BLE project:

| Raw DP9 code | Published `battery_state` | `battery_low` | `battery_critical` |
|---:|---|---|---|
| 0 | `high` | off | off |
| 1 | `normal` | off | off |
| 2 | `low` | on | off |
| 3 | `low` | on | on |

Important: the upstream mapping intentionally maps both raw codes **2 and 3** to `low`. The exact semantic difference between 2 and 3 has not been confirmed for Gimdow. `battery_critical` therefore means specifically **"DP9 raw code is 3"**, not a guaranteed documented Tuya "critical" level.

This is useful for testing the lock's audible low-battery warning: if the lock starts beeping, compare that moment with `battery_state_code`. If it changes to 3, then code 3 can safely be used as the critical/beeping threshold for that particular lock.

The diagnostic entities can be used directly on a Home Assistant dashboard or as triggers for notifications.

## BLE connection mode

If the original physical BLE remote must also be able to connect, use:

```yaml
auto_connect: false
```

The component connects on demand for a Home Assistant lock/unlock command.

If ESP32 is the only BLE controller and minimum latency matters, `auto_connect: true` may be used, but a persistent ESP32 connection can prevent another BLE remote from connecting.

## Optional physical state sensor

If `state_sensor` is configured, that sensor is authoritative:

- sensor ON = unlocked
- sensor OFF = locked

Protocol-reported state and optimistic state changes do not overwrite the external physical state.

## Credits / protocol reference

The A1 Ultra / `hc7n0urm` FD50 implementation was derived from the reverse-engineered and physically verified protocol work in the ShonP40/Tuya-BLE project, especially its Raykube A1 Ultra FD50 documentation and implementation.
