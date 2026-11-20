# CSI-2 receiver driver. neon_csi.h is the public API; platform/<family>/neon_csi.c implements it.
# The D-PHY link is driven through mipi/neon_mipi.h (NEON_MDF_DRIVER_CSI selects NEON_MDF_DRIVER_MIPI).
if(CONFIG_NEON_MDF_DRIVER_CSI)
  neon_mdf_driver_select_family_source(
    REQUIRED TARGET neon_mdf_driver PERIPHERAL csi
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/neon_csi.c)
endif()
