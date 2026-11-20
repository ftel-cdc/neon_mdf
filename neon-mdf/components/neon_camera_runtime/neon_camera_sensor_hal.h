#ifndef NEON_CAMERA_SENSOR_HAL_H
#define NEON_CAMERA_SENSOR_HAL_H

#include <stdint.h>

/*
 * Caller-owned opaque storage for the sensor selected in Kconfig.  Its layout
 * is deliberately not a sensor ABI: IMX/OS implementations remain private.
 */
typedef struct {
    uintptr_t private_words[8];
} neon_camera_sensor_hal_instance_t;

typedef struct {
    void *context;
    int (*master_transmit)(void *context, uint16_t device_address,
                           const uint8_t *data, uint16_t size,
                           uint32_t timeout_polls);
} neon_camera_sensor_i2c_bus_t;

int neon_camera_sensor_i2c_write_reg16(const neon_camera_sensor_i2c_bus_t *bus,
                                       uint16_t device_address, uint16_t register_address,
                                       uint8_t value, uint32_t timeout_polls);

typedef struct {
    int (*probe)(void *sensor);
    int (*configure_mode)(void *sensor, uint32_t mode_id);
    int (*start_stream)(void *sensor);
    int (*stop_stream)(void *sensor);
    int (*set_exposure_lines)(void *sensor, uint32_t exposure_lines, uint32_t target_frame_id);
    int (*set_analogue_gain_q8)(void *sensor, uint16_t gain_q8, uint32_t target_frame_id);
    int (*set_digital_gain_q8)(void *sensor, uint16_t gain_q8, uint32_t target_frame_id);
    uint32_t (*control_latency_frames)(void *sensor);
} neon_camera_sensor_hal_ops_t;

/* Returns the one sensor implementation selected by the application's Kconfig. */
const neon_camera_sensor_hal_ops_t *neon_camera_sensor_hal_selected_ops(void);
void neon_camera_sensor_hal_selected_init(neon_camera_sensor_hal_instance_t *instance,
                                          const neon_camera_sensor_i2c_bus_t *i2c_bus);

#endif
