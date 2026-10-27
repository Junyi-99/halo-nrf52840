#
# halo-matter: Matter over Thread colour-temperature light that drives a BenQ
# ScreenBar Halo through the nRF52840 radio in MPSL timeslots.
# Based on nRF Connect SDK samples/matter/light_bulb (LicenseRef-Nordic-5-Clause).
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(halo-matter)

include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

target_include_directories(app PRIVATE
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${ZAP_PARENT_DIR}
)

target_sources(app PRIVATE
  src/app_task.cpp
  src/main.cpp
  src/zcl_callbacks.cpp
  src/halo_radio_ts.c
  src/halo_shell.cpp
  src/crash_box.c
  src/env_sensor.cpp
  src/ac_link.cpp
  src/ac_control.cpp
  ${CHIP_ROOT}/src/app/persistence/DeferredAttributePersistenceProvider.cpp
)

ncs_configure_data_model()
