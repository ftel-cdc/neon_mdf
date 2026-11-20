#include "neon_camera_sensor_hal.h"

#include <string.h>

typedef struct {
    const neon_camera_sensor_i2c_bus_t *i2c_bus;
    uint32_t mode_id;
    uint32_t exposure_lines;
    uint32_t target_frame_id;
    uint16_t analogue_gain_q8;
    uint16_t digital_gain_q8;
    uint8_t configured;
    uint8_t streaming;
} imx675_state_t;

_Static_assert(sizeof(imx675_state_t) <= sizeof(neon_camera_sensor_hal_instance_t),
               "Increase generic sensor HAL storage");

/* Review-only register sequence. Replace with the sensor vendor mode table. */
static int probe(void *sensor) { return sensor == 0 ? -1 : 0; }

static int configure_mode(void *sensor, uint32_t mode_id)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || mode_id != 1u ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0100u, 0x00u, 1000u)) return -1;
    state->mode_id = mode_id;
    state->configured = 1u;
    return 0;
}

static int start_stream(void *sensor)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || !state->configured || state->streaming ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0100u, 0x01u, 1000u)) return -1;
    state->streaming = 1u;
    return 0;
}

static int stop_stream(void *sensor)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || !state->streaming ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0100u, 0x00u, 1000u)) return -1;
    state->streaming = 0u;
    return 0;
}

static int set_exposure(void *sensor, uint32_t value, uint32_t frame)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || !state->streaming || value < 4u || value > 4000u ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0202u, (uint8_t)(value >> 8), 1000u) ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0203u, (uint8_t)value, 1000u)) return -1;
    state->exposure_lines = value;
    state->target_frame_id = frame;
    return 0;
}

static int set_analogue_gain(void *sensor, uint16_t value, uint32_t frame)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || !state->streaming || value < 256u || value > 2048u ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0204u, (uint8_t)(value >> 8), 1000u) ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x0205u, (uint8_t)value, 1000u)) return -1;
    state->analogue_gain_q8 = value;
    state->target_frame_id = frame;
    return 0;
}

static int set_digital_gain(void *sensor, uint16_t value, uint32_t frame)
{
    imx675_state_t *state = sensor;
    if (state == 0 || state->i2c_bus == 0 || !state->streaming || value != 256u ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x020eu, (uint8_t)(value >> 8), 1000u) ||
        neon_camera_sensor_i2c_write_reg16(state->i2c_bus, 0x1au, 0x020fu, (uint8_t)value, 1000u)) return -1;
    state->digital_gain_q8 = value;
    state->target_frame_id = frame;
    return 0;
}

static uint32_t control_latency_frames(void *sensor)
{
    (void)sensor;
    return 2u;
}

void neon_camera_sensor_hal_selected_init(neon_camera_sensor_hal_instance_t *instance,
                                          const neon_camera_sensor_i2c_bus_t *i2c_bus)
{
    imx675_state_t *state = (imx675_state_t *)instance;
    if (state == 0) return;
    memset(state, 0, sizeof(*state));
    state->i2c_bus = i2c_bus;
    state->exposure_lines = 400u;
    state->analogue_gain_q8 = 256u;
    state->digital_gain_q8 = 256u;
}

const neon_camera_sensor_hal_ops_t *neon_camera_sensor_hal_selected_ops(void)
{
    static const neon_camera_sensor_hal_ops_t ops = {
        .probe = probe,
        .configure_mode = configure_mode,
        .start_stream = start_stream,
        .stop_stream = stop_stream,
        .set_exposure_lines = set_exposure,
        .set_analogue_gain_q8 = set_analogue_gain,
        .set_digital_gain_q8 = set_digital_gain,
        .control_latency_frames = control_latency_frames,
    };
    return &ops;
}
