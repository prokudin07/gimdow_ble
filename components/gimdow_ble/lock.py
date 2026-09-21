import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.components import (
    lock,
    ble_client,
    binary_sensor,
    sensor,
    text_sensor,
    select,
)
from esphome.components.ble_client import CONF_BLE_CLIENT_ID
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["lock", "binary_sensor", "sensor", "text_sensor", "select"]

gimdow_ble_ns = cg.esphome_ns.namespace("gimdow_ble")

GimdowBLELock = gimdow_ble_ns.class_(
    "GimdowBLELock",
    lock.Lock,
    ble_client.BLEClientNode,
    cg.Component,
)

GimdowConfigSelect = gimdow_ble_ns.class_(
    "GimdowConfigSelect",
    select.Select,
)

CONF_MODEL = "model"
CONF_LOCAL_KEY = "local_key"
CONF_UUID = "uuid"
CONF_DEVICE_ID = "tuya_device_id"
CONF_BLE_UNLOCK_CHECK = "ble_unlock_check"
CONF_STATE_SENSOR = "state_sensor"

CONF_BATTERY_STATE = "battery_state"
CONF_BATTERY_STATE_CODE = "battery_state_code"
CONF_BATTERY_LOW = "battery_low"
CONF_BATTERY_CRITICAL = "battery_critical"
CONF_BEEP_VOLUME = "beep_volume"
CONF_LOCK_DIRECTION = "lock_direction"

MODEL_A1_PRO_MAX = "a1_pro_max"
MODEL_A1_ULTRA = "a1_ultra"

MODEL_IDS = {
    MODEL_A1_PRO_MAX: 0,
    MODEL_A1_ULTRA: 1,
}


def _validate_model_options(config):
    if config[CONF_MODEL] == MODEL_A1_ULTRA and CONF_BLE_UNLOCK_CHECK not in config:
        raise cv.Invalid(
            "model 'a1_ultra' requires the device-specific 'ble_unlock_check' value"
        )
    return config


CONFIG_SCHEMA = cv.All(
    lock.lock_schema(GimdowBLELock)
    .extend(
        {
            cv.GenerateID(CONF_BLE_CLIENT_ID): cv.use_id(
                ble_client.BLEClient
            ),
            cv.Required(CONF_MODEL): cv.one_of(*MODEL_IDS, lower=True),
            cv.Required(CONF_LOCAL_KEY): cv.string,
            cv.Required(CONF_UUID): cv.string,
            cv.Required(CONF_DEVICE_ID): cv.string,
            cv.Optional(CONF_BLE_UNLOCK_CHECK): cv.string,
            cv.Optional(CONF_STATE_SENSOR): cv.use_id(
                binary_sensor.BinarySensor
            ),
            cv.Optional(CONF_BATTERY_STATE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_BATTERY_STATE_CODE): sensor.sensor_schema(
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_BATTERY_LOW): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_BATTERY_CRITICAL): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_BEEP_VOLUME): select.select_schema(GimdowConfigSelect),
            cv.Optional(CONF_LOCK_DIRECTION): select.select_schema(GimdowConfigSelect),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_model_options,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])

    await cg.register_component(var, config)
    await lock.register_lock(var, config)

    parent = await cg.get_variable(config[CONF_BLE_CLIENT_ID])

    cg.add(parent.register_ble_node(var))
    cg.add(var.set_parent(parent))

    cg.add(var.set_model(MODEL_IDS[config[CONF_MODEL]]))
    cg.add(var.set_local_key(config[CONF_LOCAL_KEY]))
    cg.add(var.set_uuid(config[CONF_UUID]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_BLE_UNLOCK_CHECK in config:
        cg.add(var.set_ble_unlock_check(config[CONF_BLE_UNLOCK_CHECK]))

    if CONF_STATE_SENSOR in config:
        state_sensor = await cg.get_variable(config[CONF_STATE_SENSOR])
        cg.add(var.set_state_sensor(state_sensor))

    if CONF_BATTERY_STATE in config:
        battery_state = await text_sensor.new_text_sensor(
            config[CONF_BATTERY_STATE]
        )
        cg.add(var.set_battery_state_sensor(battery_state))

    if CONF_BATTERY_STATE_CODE in config:
        battery_state_code = await sensor.new_sensor(
            config[CONF_BATTERY_STATE_CODE]
        )
        cg.add(var.set_battery_state_code_sensor(battery_state_code))

    if CONF_BATTERY_LOW in config:
        battery_low = await binary_sensor.new_binary_sensor(
            config[CONF_BATTERY_LOW]
        )
        cg.add(var.set_battery_low_sensor(battery_low))

    if CONF_BATTERY_CRITICAL in config:
        battery_critical = await binary_sensor.new_binary_sensor(
            config[CONF_BATTERY_CRITICAL]
        )
        cg.add(var.set_battery_critical_sensor(battery_critical))

    if CONF_BEEP_VOLUME in config:
        beep_volume = await select.new_select(
            config[CONF_BEEP_VOLUME],
            options=["mute", "low", "normal", "high"],
        )
        cg.add(beep_volume.set_parent(var))
        cg.add(beep_volume.set_dp_id(31))
        cg.add(var.set_beep_volume_select(beep_volume))

    if CONF_LOCK_DIRECTION in config:
        lock_direction = await select.new_select(
            config[CONF_LOCK_DIRECTION],
            options=["clockwise", "anticlockwise"],
        )
        cg.add(lock_direction.set_parent(var))
        cg.add(lock_direction.set_dp_id(48))
        cg.add(var.set_lock_direction_select(lock_direction))
