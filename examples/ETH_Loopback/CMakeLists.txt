cmake_minimum_required(VERSION 3.25)
project(eth_loopback C ASM)
get_filename_component(NEON_MDF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../neon-mdf" ABSOLUTE)
include(${NEON_MDF_PATH}/tools/cmake/project.cmake)
if(NOT NEON_MDF_TARGET_${NEON_MDF_TARGET}_FAMILY STREQUAL "FX1")
  message(FATAL_ERROR "ETH_Loopback currently requires FX1")
endif()
include(${CMAKE_BINARY_DIR}/config/sdkconfig.cmake)
if(NOT CONFIG_NEON_MDF_DRIVER_ETH)
  message(FATAL_ERROR "ETH_Loopback requires CONFIG_NEON_MDF_DRIVER_ETH=y")
endif()
neon_mdf_add_executable(eth_loopback Src/main.c)
include(${NEON_MDF_PATH}/cmake/runners/fx1-vp.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(run-vp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_vp.py
    --vp "${NEON_FX1_VP_EXECUTABLE}"
    --elf $<TARGET_FILE:eth_loopback>
    --output ${CMAKE_CURRENT_BINARY_DIR}/vp
  DEPENDS eth_loopback
  USES_TERMINAL VERBATIM
)
