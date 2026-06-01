"""ESPHome external component for the Qingping PM module.

Wires the post-SWD-patch 32-byte BM histogram frame (see qingping_pm.h for the
verified frame layout) to ESPHome sensors. All sub-sensors are optional —
declare only the ones you want exposed to Home Assistant.

Minimal YAML:

    external_components:
      - source:
          type: local
          path: components

    uart:
      id: uart_bus
      rx_pin: GPIO27
      baud_rate: 9600
      rx_buffer_size: 256

    qingping_pm:
      uart_id: uart_bus
      bin_0:  { name: "Bin 0 (>3100)" }
      bin_10: { name: "Bin 10 (TH+50)" }
      baseline:        { name: "Detector baseline" }
      frames_received: { name: "BM frames received" }
      bad_checksums:   { name: "BM bad-checksum count" }
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, uart
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

CODEOWNERS = ["@h0m3us3r"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

qingping_pm_ns = cg.esphome_ns.namespace("qingping_pm")
QingpingPM = qingping_pm_ns.class_("QingpingPM", cg.Component, uart.UARTDevice)

NUM_BINS = 11
BIN_KEYS = [f"bin_{i}" for i in range(NUM_BINS)]

CONF_BASELINE = "baseline"
CONF_FRAMES_RECEIVED = "frames_received"
CONF_BAD_CHECKSUMS = "bad_checksums"
CONF_PARTICLE_RATE = "particle_rate"
CONF_PARTICLE_RATE_DELTA = "particle_rate_delta"

_BIN_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
)

_BASELINE_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
)

_DIAG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

_RATE_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    unit_of_measurement="counts/s",
)

_RATE_DELTA_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    unit_of_measurement="counts/s²",
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(QingpingPM),
            **{cv.Optional(k): _BIN_SCHEMA for k in BIN_KEYS},
            cv.Optional(CONF_BASELINE): _BASELINE_SCHEMA,
            cv.Optional(CONF_FRAMES_RECEIVED): _DIAG_SCHEMA,
            cv.Optional(CONF_BAD_CHECKSUMS): _DIAG_SCHEMA,
            cv.Optional(CONF_PARTICLE_RATE): _RATE_SCHEMA,
            cv.Optional(CONF_PARTICLE_RATE_DELTA): _RATE_DELTA_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    for i, key in enumerate(BIN_KEYS):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(var.set_bin(i, s))

    for key, setter in (
        (CONF_BASELINE, var.set_baseline),
        (CONF_FRAMES_RECEIVED, var.set_frames_received),
        (CONF_BAD_CHECKSUMS, var.set_bad_checksums),
        (CONF_PARTICLE_RATE, var.set_particle_rate),
        (CONF_PARTICLE_RATE_DELTA, var.set_particle_rate_delta),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(setter(s))
