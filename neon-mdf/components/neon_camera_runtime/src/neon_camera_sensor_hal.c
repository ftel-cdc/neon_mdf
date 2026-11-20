#include "neon_camera_sensor_hal.h"

int neon_camera_sensor_i2c_write_reg16(const neon_camera_sensor_i2c_bus_t *bus,
                                       uint16_t device_address, uint16_t register_address,
                                       uint8_t value, uint32_t timeout_polls)
{
    const uint8_t transaction[3] = {
        (uint8_t)(register_address >> 8),
        (uint8_t)register_address,
        value,
    };

    if (bus == 0 || bus->master_transmit == 0 || device_address > 0x7fu) return -1;
    return bus->master_transmit(bus->context, device_address, transaction,
                                (uint16_t)sizeof(transaction), timeout_polls);
}
