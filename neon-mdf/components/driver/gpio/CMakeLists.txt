if(NOT NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
  neon_mdf_driver_select_family_source(
    TARGET neon_mdf_driver
    PERIPHERAL gpio
    REQUIRED
    SOURCES
      FU2        ${CMAKE_CURRENT_LIST_DIR}/platform/fu2/neon_gpio.c
      STM32F103  ${CMAKE_CURRENT_LIST_DIR}/platform/stm32f103/neon_gpio.c
      WEACT_F412 ${CMAKE_CURRENT_LIST_DIR}/platform/weact_f412/neon_gpio.c
      CH32V003   ${CMAKE_CURRENT_LIST_DIR}/platform/ch32v003/neon_gpio.c
  )
endif()
