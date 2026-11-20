cmake_minimum_required(VERSION 3.25)
project(fx1_isp_image C ASM)
get_filename_component(NEON_MDF_PATH "${CMAKE_CURRENT_LIST_DIR}/../../neon-mdf" ABSOLUTE)
include("${NEON_MDF_PATH}/tools/cmake/project.cmake")
if(NOT NEON_MDF_TARGET STREQUAL "FX1" OR NOT TARGET neon_mdf_ftel_isp)
  message(FATAL_ERROR "Use preset FX1 and CONFIG_NEON_MDF_FTEL_ISP=y")
endif()

neon_mdf_add_executable(fx1_isp_image Src/main.c)
target_link_libraries(fx1_isp_image PRIVATE neon_mdf_ftel_isp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FX1_ISP_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/profile/profile.csrw" CACHE FILEPATH "Neon application tuning profile")
set(_profile_header "${CMAKE_CURRENT_BINARY_DIR}/generated/profile.h")
add_custom_command(OUTPUT "${_profile_header}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_profile.py"
    --profile "${FX1_ISP_PROFILE}"
    --output "${_profile_header}"
  DEPENDS "${FX1_ISP_PROFILE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_profile.py"
  VERBATIM)
target_sources(fx1_isp_image PRIVATE "${_profile_header}")
target_include_directories(fx1_isp_image PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(FX1_IMAGE_FRAME_COUNT "" CACHE STRING "Override frame count; four DMA slots are reused")
if(NOT FX1_IMAGE_FRAME_COUNT STREQUAL "")
  if(NOT FX1_IMAGE_FRAME_COUNT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "FX1_IMAGE_FRAME_COUNT must be a positive integer")
  endif()
  target_compile_definitions(fx1_isp_image PRIVATE FRAME_COUNT=${FX1_IMAGE_FRAME_COUNT})
endif()


if(TARGET neon_mdf_isp_3a_binary)
  # Strict mode validates the camera mode. Replay is an explicit diagnostic
  # option because this example loads fixed BGGR RAW from DDR, not a sensor.
  option(FX1_3A_REPLAY "Allow IMX675 tuning with the example RAW replay geometry" ON)
  option(FX1_3A_REPLAY_UNSCREENED_AWB "Replay only: disable AWB ROI screening without scalar UE/OE" OFF)
  if(FX1_3A_REPLAY_UNSCREENED_AWB AND NOT FX1_3A_REPLAY)
    message(FATAL_ERROR "Unscreened AWB is allowed only with FX1_3A_REPLAY")
  endif()
  target_compile_definitions(fx1_isp_image PRIVATE
    FX1_3A_REPLAY=$<BOOL:${FX1_3A_REPLAY}>
    FX1_3A_REPLAY_UNSCREENED_AWB=$<BOOL:${FX1_3A_REPLAY_UNSCREENED_AWB}>)
  set(FX1_3A_SENSOR_BIN "${CMAKE_CURRENT_SOURCE_DIR}/profile/fpt.sensormodule.sony_imx675.bin" CACHE FILEPATH "Sensor module binary")
  set(FX1_3A_TUNING_BIN "${CMAKE_CURRENT_SOURCE_DIR}/profile/fpt.tuned.sony_imx675.bin" CACHE FILEPATH "3A tuning binary")
  # Stable linker symbols without encoding absolute filenames in the ABI.
  foreach(_kind sensor tuning)
    string(TOUPPER "${_kind}" _upper)
    get_filename_component(_bin "${FX1_3A_${_upper}_BIN}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_bin}")
      message(FATAL_ERROR "Missing camera binary: ${_bin}")
    endif()
    set(_asm "${CMAKE_CURRENT_BINARY_DIR}/generated/3a_${_kind}.S")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(CONFIGURE OUTPUT "${_asm}" CONTENT
      ".section .rodata.fpt_${_kind},\"a\",@progbits\n.balign 8\n.global fpt_${_kind}_start\nfpt_${_kind}_start:\n.incbin \"${_bin}\"\n.global fpt_${_kind}_end\nfpt_${_kind}_end:\n" @ONLY)
    set_source_files_properties("${_asm}" PROPERTIES OBJECT_DEPENDS "${_bin}")
    target_sources(fx1_isp_image PRIVATE "${_asm}")
  endforeach()

  # Separate VP test executable. The normal firmware/component stays unchanged.
  get_target_property(_image_sources fx1_isp_image SOURCES)
  get_target_property(_image_definitions fx1_isp_image COMPILE_DEFINITIONS)
  neon_mdf_add_executable(fx1_isp_image_vp ${_image_sources} Tests/vp_awb_counts.c)
  set_target_properties(fx1_isp_image_vp PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_link_libraries(fx1_isp_image_vp PRIVATE neon_mdf_ftel_isp)
  target_include_directories(fx1_isp_image_vp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${NEON_MDF_PATH}/components/ftel_isp/pipeline"
    "${NEON_MDF_PATH}/components/ftel_isp/algorithms/isp_3a/include")
  target_compile_definitions(fx1_isp_image_vp PRIVATE ${_image_definitions})
  target_link_options(fx1_isp_image_vp PRIVATE -Wl,--wrap=ftel_isp_auto_collect)
  if(FX1_IMAGE_FRAME_COUNT GREATER 4)
    target_compile_definitions(fx1_isp_image_vp PRIVATE VP_CAPTURE_FRAME_COUNT=${FX1_IMAGE_FRAME_COUNT})
    target_link_options(fx1_isp_image_vp PRIVATE -Wl,--wrap=isp_stream_submit)
  endif()
endif()
