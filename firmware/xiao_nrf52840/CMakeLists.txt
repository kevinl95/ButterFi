cmake_minimum_required(VERSION 3.20.0)

set(BUTTERFI_USB_CONTROL_DEBUG_DEFAULT ON)
if(DEFINED ENV{BUTTERFI_USB_CONTROL_DEBUG})
    set(BUTTERFI_USB_CONTROL_DEBUG_DEFAULT $ENV{BUTTERFI_USB_CONTROL_DEBUG})
endif()

set(BUTTERFI_INCLUDE_SIDEWALK_DEFAULT OFF)
if(DEFINED ENV{BUTTERFI_INCLUDE_SIDEWALK})
    set(BUTTERFI_INCLUDE_SIDEWALK_DEFAULT $ENV{BUTTERFI_INCLUDE_SIDEWALK})
endif()

set(BUTTERFI_DISABLE_SIDEWALK_ASSERTS_DEFAULT OFF)
if(DEFINED ENV{BUTTERFI_DISABLE_SIDEWALK_ASSERTS})
    set(BUTTERFI_DISABLE_SIDEWALK_ASSERTS_DEFAULT $ENV{BUTTERFI_DISABLE_SIDEWALK_ASSERTS})
endif()

option(BUTTERFI_USB_CONTROL_DEBUG "Build the USB-only control runtime" ${BUTTERFI_USB_CONTROL_DEBUG_DEFAULT})
option(BUTTERFI_INCLUDE_SIDEWALK "Compile in the Sidewalk SDK and related sources" ${BUTTERFI_INCLUDE_SIDEWALK_DEFAULT})
option(BUTTERFI_DISABLE_SIDEWALK_ASSERTS "Compile the Sidewalk PAL with asserts disabled" ${BUTTERFI_DISABLE_SIDEWALK_ASSERTS_DEFAULT})

# Point at Amazon Sidewalk SDK before finding Zephyr
# Set SIDEWALK_BASE in your environment:
#   export SIDEWALK_BASE=$HOME/sidewalk
if(BUTTERFI_INCLUDE_SIDEWALK)
    list(APPEND ZEPHYR_EXTRA_MODULES $ENV{SIDEWALK_BASE})
endif()

find_package(Zephyr REQUIRED HINTS $ENV{BUTTERFI_ZEPHYR_BASE_HINT} $ENV{ZEPHYR_BASE})
project(butterfi VERSION 1.0.0)

if(BUTTERFI_DISABLE_SIDEWALK_ASSERTS)
    zephyr_compile_definitions(SID_PAL_ASSERT_DISABLED)
endif()

target_compile_definitions(app PRIVATE BUTTERFI_VERSION=\"${PROJECT_VERSION}\")

if(BUTTERFI_USB_CONTROL_DEBUG)
    target_compile_definitions(app PRIVATE BUTTERFI_USB_CONTROL_DEBUG=1)
else()
    target_compile_definitions(app PRIVATE BUTTERFI_USB_CONTROL_DEBUG=0)
endif()

if(BUTTERFI_INCLUDE_SIDEWALK)
    target_compile_definitions(app PRIVATE BUTTERFI_INCLUDE_SIDEWALK=1)
else()
    target_compile_definitions(app PRIVATE BUTTERFI_INCLUDE_SIDEWALK=0)
endif()

target_sources(app PRIVATE
    src/main.c
    src/butterfi_config.c
    src/butterfi_usb.c
)

if(BUTTERFI_INCLUDE_SIDEWALK)
    target_sources(app PRIVATE src/butterfi_content.c)
endif()

target_include_directories(app PRIVATE src)
