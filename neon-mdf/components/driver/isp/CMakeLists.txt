# ISP HAL. The headers outside platform/ are the public API.
#   platform/common/neon_isp.c    every rule of the module, the same for every chip; it calls only the
#                                  neon_isp_hw_*() contract of platform/common/neon_isp_hw.h
#   platform/<family>/             what a chip adds: fx1_isp_caps.c (its limits), fx1_isp_profile.c (what a tuning
#                                  profile may write) and exactly one backend,
#                                  fx1_isp_simulation.c or the future register backend
# A new chip adds its own platform/<family>/ and one pair per list below; the common file is listed for it too.
#
# A build that only enables the ftel_isp component (NEON_MDF_ENABLE_FTEL_ISP) has no client of this HAL,
# but neon_mdf_driver must not be empty, so the simulation backend is compiled for it as before.
if(CONFIG_NEON_MDF_DRIVER_ISP OR NEON_MDF_ENABLE_FTEL_ISP)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL isp
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/common/neon_isp.c)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL isp
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/fx1_isp_caps.c)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL isp
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/fx1_isp_profile.c)
  if(CONFIG_NEON_MDF_CAMERA_SIMULATION OR NOT CONFIG_NEON_MDF_DRIVER_ISP)
    neon_mdf_driver_select_family_source(
      REQUIRED TARGET neon_mdf_driver PERIPHERAL isp
      SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/fx1_isp_simulation.c)
  else()
    message(FATAL_ERROR
      "components/driver/isp: the FX1 register backend does not exist yet. It can be built on the "
      "ftel_isp component (its register access and ISP_EN, DMA and statistics sequences), but ftel_isp "
      "runs one frame at a time and does not yet have the rotating DMA this HAL needs. "
      "Enable NEON_MDF_CAMERA_SIMULATION.")
  endif()
endif()
