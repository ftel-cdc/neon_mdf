# MIPI D-PHY link HAL. The header outside platform/ is the public API.
#   platform/common/neon_mipi.c   every rule of the module, the same for every chip; it calls only the
#                                  neon_mipi_hw_*() contract of platform/common/neon_mipi_hw.h
#   platform/<family>/             what a chip adds: fx1_mipi_caps.c (its limits) and exactly one backend,
#                                  fx1_mipi_simulation.c or the future register backend
# A new chip adds its own platform/<family>/ and one pair per list below; the common file is listed for it too.
if(CONFIG_NEON_MDF_DRIVER_MIPI)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL mipi
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/common/neon_mipi.c)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL mipi
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/fx1_mipi_caps.c)
  if(CONFIG_NEON_MDF_CAMERA_SIMULATION)
    neon_mdf_driver_select_family_source(
      REQUIRED TARGET neon_mdf_driver PERIPHERAL mipi
      SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/fx1_mipi_simulation.c)
  else()
    message(FATAL_ERROR
      "components/driver/mipi: the FX1 register backend does not exist yet. It needs the register "
      "workbook FTEL_MEMORY_MAP_REGISTERS (v1.4 or later). Enable NEON_MDF_CAMERA_SIMULATION.")
  endif()
endif()
