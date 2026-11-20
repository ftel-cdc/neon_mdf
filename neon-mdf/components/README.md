# FTEL camera components

These components are the FX1 camera-stack contract.  They deliberately do
not reuse `ftel_isp_bu/`: that directory remains an independent backup of the
previous monolithic ISP import while this layout is reviewed.

```text
components/driver/
  ├── mipi                      D-PHY ownership, power and lane driver
  ├── csi                       CSI-2 receive and RAW routing driver
  └── isp                       ISP pipeline and decoded-statistics driver

neon_camera_runtime
  ├── 3a                        AE/AWB/AF policy and profile adapter
  └── sensors/                  selected sensor implementation (private)
```

`neon_camera_runtime` owns one camera pipeline and coordinates startup,
frame tags and shutdown. `neon_camera_runtime_init()` gathers one camera description (sensor, geometry, lanes,
one DDR region) into consistent mipi, csi and isp configurations, and takes the optional tuning
binaries: an ISP register profile checked against the register map, and the 3A tuning.  It does not own MIPI/CSI/ISP registers and does not
know sensor I2C register addresses. The `3a/` module is an algorithm
component, not a FreeRTOS task; a camera worker invokes it after statistics
have been decoded.

`mipi`, `csi` and `isp` are documented in `guide/FX1_CAMERA_HAL.md` and have host tests under
`tests/camera_hal`. All three are direct-function HALs on a `neon_status_t` contract with the same layout
(public header, `platform/common` core, `platform/fx1` caps and simulation). None has a register backend yet;
the guide lists what is still open.

The reference implementation adds a deterministic in-RAM simulation to each
layer plus IMX675 and OS04C10 sensor HAL examples. It is intentionally not an
FX1 register driver: no register address, D-PHY timing, CSI packet bitfield or
sensor I2C sequence is invented here. The camera runtime has a small
reference AE/AWB policy with the same decoded-stats-to-semantic-controls shape
as `3A_tuning_algorithm`; the latter cannot yet be linked directly because its
new `aec_extended` tuning ABI header is not included with the supplied source.

The public sensor contract is `neon_camera_sensor_hal.h`.  The application owns a
generic `neon_camera_sensor_hal_instance_t`, then initializes it and obtains the
selected operations through `neon_camera_sensor_hal_selected_*()`.  IMX675 and
OS04C10 state layout and register implementation remain private.

Sensor HALs transmit all mode/stream/exposure/gain commands through
`neon_camera_sensor_i2c_bus_t`, whose transaction shape matches
`neon_i2c_master_transmit`. The existing Neon MDF I2C implementation is FU2
only, so FX1 must add its `components/driver/i2c/platform/fx1` backend and bind
that backend to this bus before the reference sensor drivers can talk to real
hardware.
