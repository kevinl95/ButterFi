#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WITH_ENV_SCRIPT="$SCRIPT_DIR/with-ncs-env.sh"

APP_DIR="${BUTTERFI_APP:-$REPO_ROOT/firmware/xiao_nrf52840}"
BOARD="${BUTTERFI_BOARD:-xiao_ble}"
BUILD_DIR="${BUTTERFI_BUILD_DIR:-$APP_DIR/build}"
ENV_JSON="${NCS_ENV_JSON:-}"
ZEPHYR_BASE_DIR="${ZEPHYR_BASE:-}"
EXTRA_CONF_FILE="${BUTTERFI_EXTRA_CONF_FILE:-}"
USB_CONTROL_DEBUG="${BUTTERFI_USB_CONTROL_DEBUG:-}"
INCLUDE_SIDEWALK="${BUTTERFI_INCLUDE_SIDEWALK:-}"
DISABLE_SIDEWALK_ASSERTS="${BUTTERFI_DISABLE_SIDEWALK_ASSERTS:-}"

usage() {
    cat <<EOF
Usage: build-xiao.sh <action>

Actions:
  configure  Run a pristine west build for the XIAO firmware
  build      Run incremental ninja build in the existing build directory
  rebuild    Run configure followed by build
  help       Show this message

Environment overrides:
  NCS_ENV_JSON         Path to toolchain environment.json
  NCS_TOOLCHAIN_DIR    Toolchain root containing environment.json
  SIDEWALK_BASE        Path to the local sdk-sidewalk checkout
    ZEPHYR_BASE          Path to the Zephyr tree inside your NCS workspace
  BUTTERFI_APP         App path (default: REPO_ROOT/firmware/xiao_nrf52840)
  BUTTERFI_BOARD       Zephyr board name (default: xiao_ble)
  BUTTERFI_BUILD_DIR   Build output directory (default: APP_DIR/build)
    BUTTERFI_EXTRA_CONF_FILE  Extra Kconfig fragment merged at configure time
    BUTTERFI_USB_CONTROL_DEBUG  Override CMake option for USB control runtime
    BUTTERFI_INCLUDE_SIDEWALK  Override CMake option for Sidewalk sources/module
    BUTTERFI_DISABLE_SIDEWALK_ASSERTS  Override CMake option for Sidewalk PAL asserts
EOF
}

run_cmd() {
    if [[ -n "$ENV_JSON" ]]; then
        "$WITH_ENV_SCRIPT" --env-json "$ENV_JSON" -- "$@"
    else
        "$WITH_ENV_SCRIPT" -- "$@"
    fi
}

require_paths() {
    local expected_sidewalk

    if [[ ! -d "$APP_DIR" ]]; then
        echo "App directory not found: $APP_DIR" >&2
        exit 1
    fi

    if [[ -z "${SIDEWALK_BASE:-}" ]]; then
        echo "SIDEWALK_BASE must point at a local sdk-sidewalk checkout" >&2
        exit 1
    fi

    if [[ -z "$ZEPHYR_BASE_DIR" ]]; then
        echo "ZEPHYR_BASE must point at the Zephyr tree inside your NCS workspace" >&2
        exit 1
    fi

    if [[ ! -d "$ZEPHYR_BASE_DIR" ]]; then
        echo "ZEPHYR_BASE directory not found: $ZEPHYR_BASE_DIR" >&2
        exit 1
    fi

    expected_sidewalk="$(dirname "$ZEPHYR_BASE_DIR")/sidewalk"
    if [[ ! -e "$expected_sidewalk" ]]; then
        echo "Sidewalk SDK must also be available at: $expected_sidewalk" >&2
        echo "Clone it there or create a symlink to: $SIDEWALK_BASE" >&2
        exit 1
    fi
}

resolve_optional_path() {
    local value="$1"

    if [[ -z "$value" ]]; then
        return 0
    fi

    if [[ "$value" = /* ]]; then
        printf '%s' "$value"
    else
        printf '%s' "$APP_DIR/$value"
    fi
}

configure_build() {
    local extra_conf_path=""
    local cmake_args=()

    require_paths

    extra_conf_path="$(resolve_optional_path "$EXTRA_CONF_FILE")"

    if [[ -n "$extra_conf_path" ]]; then
        cmake_args+=("-DEXTRA_CONF_FILE=$extra_conf_path")
    fi

    if [[ -n "$USB_CONTROL_DEBUG" ]]; then
        cmake_args+=("-DBUTTERFI_USB_CONTROL_DEBUG:BOOL=$USB_CONTROL_DEBUG")
    fi

    if [[ -n "$INCLUDE_SIDEWALK" ]]; then
        cmake_args+=("-DBUTTERFI_INCLUDE_SIDEWALK:BOOL=$INCLUDE_SIDEWALK")
    fi

    if [[ -n "$DISABLE_SIDEWALK_ASSERTS" ]]; then
        cmake_args+=("-DBUTTERFI_DISABLE_SIDEWALK_ASSERTS:BOOL=$DISABLE_SIDEWALK_ASSERTS")
    fi

    (
        cd "$REPO_ROOT"
        if [[ ${#cmake_args[@]} -gt 0 ]]; then
            run_cmd west -z "$ZEPHYR_BASE_DIR" build -d "$BUILD_DIR" -p -b "$BOARD" "$APP_DIR" -- "${cmake_args[@]}"
        else
            run_cmd west -z "$ZEPHYR_BASE_DIR" build -d "$BUILD_DIR" -p -b "$BOARD" "$APP_DIR"
        fi
    )
}

incremental_build() {
    require_paths
    if [[ ! -d "$BUILD_DIR" ]]; then
        echo "Build directory not found: $BUILD_DIR" >&2
        echo "Run the configure action first." >&2
        exit 1
    fi

    run_cmd ninja -C "$BUILD_DIR"
}

action="${1:-help}"

case "$action" in
    configure)
        configure_build
        ;;
    build)
        incremental_build
        ;;
    rebuild)
        configure_build
        incremental_build
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        echo "Unknown action: $action" >&2
        usage >&2
        exit 2
        ;;
esac