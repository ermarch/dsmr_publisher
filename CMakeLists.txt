cmake_minimum_required(VERSION 3.10)

project(DSMR_Publisher
    VERSION 1.0 
    DESCRIPTION "A Dutch Smart Meter reader and publisher"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)

# Set default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()


# Add the executable target
add_executable(dsmr_publisher
  src/config.c
  src/dsmr_parsing.c
  src/p1_serial.c
  src/prometheus.c
  src/mqtt_ha.c
  src/systemd.c
  src/main.c
)

install(TARGETS dsmr_publisher DESTINATION bin)
