if(CONFIG_NEON_MDF_CAMERA_RUNTIME)
  set(NEON_MDF_CAMERA_RUNTIME_SOURCES
    src/neon_camera_runtime.c
    src/neon_camera_runtime_init.c
    src/neon_camera_sensor_hal.c
    3a/neon_camera_3a_reference.c)
  if(CONFIG_NEON_MDF_CAMERA_SENSOR_IMX675)
    list(APPEND NEON_MDF_CAMERA_RUNTIME_SOURCES sensors/neon_camera_sensor_imx675.c)
  elseif(CONFIG_NEON_MDF_CAMERA_SENSOR_OS04C10)
    list(APPEND NEON_MDF_CAMERA_RUNTIME_SOURCES sensors/neon_camera_sensor_os04c10.c)
  endif()
  add_library(neon_mdf_camera_runtime STATIC ${NEON_MDF_CAMERA_RUNTIME_SOURCES})
  add_library(neon_mdf::camera_runtime ALIAS neon_mdf_camera_runtime)
  target_include_directories(neon_mdf_camera_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(neon_mdf_camera_runtime PUBLIC neon_mdf_driver neon_mdf_kconfig)
  target_compile_features(neon_mdf_camera_runtime PUBLIC c_std_11)
endif()
