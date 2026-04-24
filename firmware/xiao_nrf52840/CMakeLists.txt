cmake_minimum_required(VERSION 3.20.0)

# Point at Amazon Sidewalk SDK before finding Zephyr
# Set SIDEWALK_BASE in your environment:
#   export SIDEWALK_BASE=$HOME/sidewalk
list(APPEND ZEPHYR_EXTRA_MODULES $ENV{SIDEWALK_BASE})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(butterfi VERSION 1.0.0)

target_sources(app PRIVATE
    src/main.c
    src/butterfi_config.c
    src/butterfi_usb.c
    src/butterfi_content.c
)

target_include_directories(app PRIVATE src)
