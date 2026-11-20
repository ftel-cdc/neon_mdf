if(CONFIG_NEON_MDF_DRIVER_ETH)
  neon_mdf_driver_select_family_source(
    TARGET neon_mdf_driver
    PERIPHERAL eth
    SOURCES FX1 ${CMAKE_CURRENT_LIST_DIR}/platform/fx1/neon_eth.c
  )
  if(NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
    target_sources(neon_mdf_driver PRIVATE
      platform/fx1/reference/eth_drv.c
      platform/fx1/reference/fx1_eth.c
    )
  endif()
endif()
