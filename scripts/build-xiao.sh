#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WITH_ENV_SCRIPT="$SCRIPT_DIR/with-ncs-env.sh"

APP_DIR="${BUTTERFI_APP:-$REPO_ROOT/firmware/xiao_nrf52840}"
BOARD="${BUTTERFI_BOARD:-xiao_ble}"
BUILD_DIR="${BUTTERFI_BUILD_DIR:-$APP_DIR/build}"
ENV_JSON="${NCS_ENV_JSON:-}"

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
  BUTTERFI_APP         App path (default: REPO_ROOT/firmware/xiao_nrf52840)
  BUTTERFI_BOARD       Zephyr board name (default: xiao_ble)
  BUTTERFI_BUILD_DIR   Build output directory (default: APP_DIR/build)
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
    if [[ ! -d "$APP_DIR" ]]; then
        echo "App directory not found: $APP_DIR" >&2
        exit 1
    fi

    if [[ -z "${SIDEWALK_BASE:-}" ]]; then
        echo "SIDEWALK_BASE must point at a local sdk-sidewalk checkout" >&2
        exit 1
    fi
}

configure_build() {
    require_paths
    (
        cd "$REPO_ROOT"
        run_cmd west build -p -b "$BOARD" "$APP_DIR"
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