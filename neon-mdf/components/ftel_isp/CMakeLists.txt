if(NOT NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
  message(FATAL_ERROR "ftel_isp currently requires the FX1 platform")
endif()
add_library(neon_mdf_ftel_isp STATIC
  "common/ftel_isp_common.c"
  "pipeline/ftel_isp_init.c"
  "pipeline/ftel_isp_3a.c"
  "pipeline/ftel_isp_stream.c"
  "bayer domain/ifmt/ftel_isp_ifmt.c"
  "bayer domain/blc/ftel_isp_blc.c"
  "bayer domain/lsc/ftel_isp_lsc.c"
  "bayer domain/bpc/ftel_isp_bpc.c"
  "bayer domain/wb/ftel_isp_wb.c"
  "bayer domain/dg/ftel_isp_dg.c"
  "rgb domain/demosaic/ftel_isp_demosaic.c"
  "rgb domain/ccm/ftel_isp_ccm.c"
  "rgb domain/gamma/ftel_isp_gamma.c"
  "rgb domain/csc/ftel_isp_csc.c"
  "yuv domain/gtm/ftel_isp_gtm.c"
  "yuv domain/nr/ftel_isp_nr.c"
  "yuv domain/ee/ftel_isp_ee.c"
  "yuv domain/cnf/ftel_isp_cnf.c"
  "stats/aec/ftel_isp_aec.c"
  "stats/awb/ftel_isp_awb.c"
  "stats/af/ftel_isp_af.c"
  "algorithms/ae/ftel_isp_ae.c"
  "io/output/ftel_isp_output.c"
  "io/dma/ftel_isp_dma.c"
)
target_compile_features(neon_mdf_ftel_isp PUBLIC c_std_11)
target_include_directories(neon_mdf_ftel_isp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(neon_mdf_ftel_isp PUBLIC ${NEON_MDF_DEVICE_TARGET} neon_mdf_kconfig)
target_compile_definitions(neon_mdf_ftel_isp PUBLIC
  ISP_BASE=FX1_ISP_CSR_BASE ISP_CHECK_RECOVERY_IDLE=1 COMMON_RECOVERY_STATUS_BUSY=1u)
target_compile_options(neon_mdf_ftel_isp PUBLIC -include fx1/fx1_memory_map.h)

# OS-independent API used by the optional FreeRTOS service.
add_library(neon_mdf::ftel_isp ALIAS neon_mdf_ftel_isp)
target_sources(neon_mdf_ftel_isp PRIVATE
  pipeline/ftel_isp_bayer_aec.c pipeline/ftel_isp_frame.c
  platform/fx1/ftel_isp_platform.c)
target_include_directories(neon_mdf_ftel_isp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pipeline")

if(CONFIG_NEON_MDF_FTEL_ISP_3A)
  set(_3a_root "${CMAKE_CURRENT_SOURCE_DIR}/algorithms/isp_3a")
  set(_3a_archive "${_3a_root}/lib/fx1/libisp_3a.a")
  if(NOT EXISTS "${_3a_archive}")
    message(FATAL_ERROR "Missing FX1 3A distribution: ${_3a_archive}")
  endif()
  add_library(neon_mdf_isp_3a_binary STATIC IMPORTED GLOBAL)
  set_target_properties(neon_mdf_isp_3a_binary PROPERTIES IMPORTED_LOCATION "${_3a_archive}")
  target_sources(neon_mdf_ftel_isp PRIVATE pipeline/ftel_isp_auto.c stats/ftel_isp_stats_adapter.c)
  target_include_directories(neon_mdf_ftel_isp PRIVATE "${_3a_root}/include")
  target_link_libraries(neon_mdf_ftel_isp PRIVATE neon_mdf_isp_3a_binary ${NEON_MDF_DEVICE_TARGET} gcc)
  target_compile_definitions(neon_mdf_ftel_isp PUBLIC NEON_MDF_HAS_ISP_3A=1)
endif()
