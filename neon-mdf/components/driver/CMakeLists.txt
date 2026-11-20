# Driver arch-agnostic: operation via register struct created from mcu-description. Each peripheral
# selects its own backend in <peripheral>/CMakeLists.txt; target, include path and shared
# dependencies are kept here.

# Keep a real archive even when every optional FX1 peripheral is disabled.
add_library(neon_mdf_driver STATIC neon_driver.c)

target_include_directories(neon_mdf_driver PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Get CMSIS/device headers, architectural flags and the CONFIG_NEON_MDF_DRIVER_*.
target_link_libraries(neon_mdf_driver PUBLIC ${NEON_MDF_DEVICE_TARGET} neon_mdf_kconfig)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/driver_helpers.cmake)

foreach(_neon_mdf_driver_peripheral
    crm syscsr gpio flash uart i2c crc efuse adc dma spi pwm tim rtc trng aes qspi wdt eth)
  add_subdirectory(${_neon_mdf_driver_peripheral})
endforeach()

# Camera HAL backends are currently provided for FX1 only.
if(NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
  add_subdirectory(mipi)
  add_subdirectory(csi)
  add_subdirectory(isp)
endif()
