# neon-mdf/CMakeLists.txt - build in the correct "SDK" (components/driver + device target), contains
# NO app. Like ESP-IDF's $IDF_PATH/CMakeLists.txt: not stand alone, always add_subdirectory() by
# tools/cmake/project.cmake (included from CMakeLists.txt of an app).

if(NOT NEON_MDF_PATH)
  set(NEON_MDF_PATH ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(NOT NEON_MDF_TARGET)
  message(FATAL_ERROR "NEON_MDF_TARGET must be set before add_subdirectory(neon-mdf)")
endif()

include(${NEON_MDF_PATH}/cmake/mcu_variants.cmake)
if(NOT NEON_MDF_TARGET IN_LIST NEON_MDF_KNOWN_TARGETS)
  message(FATAL_ERROR "NEON_MDF_TARGET='${NEON_MDF_TARGET}' not registered in cmake/mcu_variants.cmake (valid target: ${NEON_MDF_KNOWN_TARGETS})")
endif()

neon_mdf_configure_target(${NEON_MDF_TARGET})

# PARENT_SCOPE: This file is always a subdirectory (no top-level timer), so propagate to the caller
# (the app's CMakeLists.txt)
# -T${NEON_MDF_LINKER_SCRIPT} for their executable.
set(NEON_MDF_DEVICE_TARGET ${NEON_MDF_DEVICE_TARGET} PARENT_SCOPE)
set(NEON_MDF_LINKER_SCRIPT ${NEON_MDF_LINKER_SCRIPT} PARENT_SCOPE)
# NEON_MDF_TARGET_ARCH - need for tools/cmake/project.cmake's neon_mdf_add_bootloader_executable()
# (whether know part needs more `gcc` in
# -Wl,--start-group don't, see comments there) and cmake/flashmap.cmake (--arch for gen_flashmap.py,
# see comments there).
set(NEON_MDF_TARGET_ARCH ${NEON_MDF_TARGET_ARCH} PARENT_SCOPE)
set(NEON_MDF_BOOTLOADER_ENABLED ${NEON_MDF_BOOTLOADER_ENABLED} PARENT_SCOPE)
if(NEON_MDF_BOOTLOADER_ENABLED)
  set(NEON_MDF_BOOTLOADER_LINKER_SCRIPT ${NEON_MDF_BOOTLOADER_LINKER_SCRIPT} PARENT_SCOPE)
  set(NEON_MDF_APP_LINKER_SCRIPT ${NEON_MDF_APP_LINKER_SCRIPT} PARENT_SCOPE)
  # expected for flash_jtag/flash_uart_stm32 (see cmake/flash_targets.cmake) - address/size bootloader +
  # app (factory) to write private zone 2 (no more constant blob ghep 1) via SWD (flash_jtag) or
  # real System Bootloader via UART (flash_uart_stm32).
  set(NEON_MDF_BOOTLOADER_SIZE ${NEON_MDF_BOOTLOADER_SIZE} PARENT_SCOPE)
  set(NEON_MDF_IMAGE_HEADER_SIZE ${NEON_MDF_IMAGE_HEADER_SIZE} PARENT_SCOPE)
  set(NEON_MDF_APP_BASE ${NEON_MDF_APP_BASE} PARENT_SCOPE)
  # required for tools/gen_flashmap.py (see cmake/flashmap.cmake) - PARENT_SCOPE letter level 2 (this
  # matches add_subdirectory(neon-mdf)), the same failure as the above 3 variables.
  set(NEON_MDF_RAM_BASE ${NEON_MDF_RAM_BASE} PARENT_SCOPE)
  set(NEON_MDF_RAM_SIZE ${NEON_MDF_RAM_SIZE} PARENT_SCOPE)
  set(NEON_MDF_ERASE_SECTORS ${NEON_MDF_ERASE_SECTORS} PARENT_SCOPE)
endif()

# flashmap.csv (app's own partition table, see cmake/flashmap.cmake) - optional, automatically enabled
# if the app has this file (CMAKE_SOURCE_DIR is the app directory, because the app owns the top-level project()
# top-level - not this directory) - like kconfig's osource for the app's own Kconfig, the app does
# not need to edit its own CMakeLists.txt to "enable" this feature, just having the file is
# sufficient. must call before add_subdirectory(components/bootloader) at the bottom - so that the
# components/bootloader/CMakeLists.txt link is targeted by neon_mdf_ota (created by
# neon_mdf_process_flashmap() via add_subdirectory(components/ota)).
#
# prioritize flashmap.<NEON_MDF_TARGET>.csv (override by device) if available, fallback flashmap.csv
# (default/shared) - required for app builds with multiple BOOTLOADER-enabled devices with large
# skewed flash size (e.g. Blinky: WEACT_F412 512KB vs CH32V003F4P6 16KB - cannot share 1
# flashmap.csv, see comment here in cmake/flashmap.cmake).
set(_flashmap_csv_candidate ${CMAKE_SOURCE_DIR}/flashmap.${NEON_MDF_TARGET}.csv)
if(NOT EXISTS ${_flashmap_csv_candidate})
  set(_flashmap_csv_candidate ${CMAKE_SOURCE_DIR}/flashmap.csv)
endif()
set(NEON_MDF_APP_HAS_FLASHMAP OFF)
if(NEON_MDF_BOOTLOADER_ENABLED AND EXISTS ${_flashmap_csv_candidate})
  include(${NEON_MDF_PATH}/cmake/flashmap.cmake)
  neon_mdf_process_flashmap(${CMAKE_SOURCE_DIR} ${_flashmap_csv_candidate})
  set(NEON_MDF_APP_HAS_FLASHMAP ON)
  # PARENT_SCOPE 2nd mail layer (this matches add_subdirectory(neon-mdf)) for each variable
  # NEON_MDF_FLASHMAP_* - name list is dynamic (depending on the partition in flashmap.csv of each
  # app) so must iterate through NEON_MDF_FLASHMAP_VAR_NAMES (which neon_mdf_process_flashmap() look
  # up), not hardcoded as 3 variables BOOTLOADER_SIZE/IMAGE_HEADER_SIZE/APP_BASE above.
  foreach(_flashmap_name ${NEON_MDF_FLASHMAP_VAR_NAMES})
    set(${_flashmap_name} ${${_flashmap_name}} PARENT_SCOPE)
  endforeach()
endif()
set(NEON_MDF_APP_HAS_FLASHMAP ${NEON_MDF_APP_HAS_FLASHMAP} PARENT_SCOPE)

# components/driver is written for the common register layout of the FU2 family (see ARCHITECTURE.md
# section 6) - guard by FAMILY, not the correct target name. WEACT_F412 also uses components/driver
# (a separate GPIO backend for STM32F4, see components/driver/CMakeLists.txt) so it is in the
# allowed family list. STM32F103 uses a separate GPIO backend for CRL/CRH; CH32V003 (RISC-V, WCH)
# uses its own CFGLR GPIO backend.
if(NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FU2" OR
   NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "STM32F103" OR
   NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "WEACT_F412" OR
   NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "CH32V003")
  include(${NEON_MDF_PATH}/cmake/kconfig.cmake)
  neon_mdf_process_kconfig(${CMAKE_SOURCE_DIR})

  add_subdirectory(components/driver)
  add_subdirectory(components/console)

  # FU2 has a mask-ROM first-stage loader.  Keep its policy in a separate
  # component from the optional flash-resident second-stage bootloader.
  # Applications do not link this target unless they explicitly build a ROM
  # image. Application-only SDK checkouts can omit the ROM sources and use
  # the device's existing ROM instead.
  if(NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FU2" AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/components/bootrom/CMakeLists.txt")
    add_subdirectory(components/bootrom)
  endif()

  # components/bootloader requires the above-defined neon_mdf_driver (flash) - add only when the actual
  # device is building with `bootloader:` declaration (see
  # cmake/mcu_variants.cmake).
  if(NEON_MDF_BOOTLOADER_ENABLED)
    add_subdirectory(components/bootloader)
    # NEON_MDF_BOOTLOADER_MAIN_SOURCES - PARENT_SCOPE mail layer 2 (compatible with this
    # add_subdirectory(neon-mdf)) - used by neon_mdf_add_bootloader_executable() in
    # tools/cmake/project.cmake (runs at the scope of the app) to compile directly main.c+arch file
    # as source of executable "bootloader", see
    # components/bootloader/CMakeLists.txt.
    set(NEON_MDF_BOOTLOADER_MAIN_SOURCES ${NEON_MDF_BOOTLOADER_MAIN_SOURCES} PARENT_SCOPE)
  endif()
endif()

# FX1 uses the bundled CLINT/PLIC BSP, with no MCU GPIO HAL dependency.
if(NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
  include(${NEON_MDF_PATH}/cmake/kconfig.cmake)
  neon_mdf_process_kconfig(${CMAKE_SOURCE_DIR})
  add_subdirectory(components/driver)
  add_subdirectory(components/neon_camera_runtime)
  if(CONFIG_NEON_MDF_FTEL_ISP OR NEON_MDF_ENABLE_FTEL_ISP)
    add_subdirectory(components/ftel_isp)
  endif()
endif()
