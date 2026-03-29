#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WITH_ENV_SCRIPT="$SCRIPT_DIR/with-ncs-env.sh"

VENDOR_ROOT="${BUTTERFI_VENDOR_ROOT:-$REPO_ROOT/vendor}"
MANIFEST_REPO="${BUTTERFI_MANIFEST_REPO:-$VENDOR_ROOT/RAK4630-Amazon-Sidewalk-Example}"
BOARD="${BUTTERFI_BOARD:-rak4631}"
APP="${BUTTERFI_APP:-app/rak4631_rak1901_demo}"
OVERLAY="${BUTTERFI_OVERLAY:-lora.conf}"
BUILD_DIR="${BUTTERFI_BUILD_DIR:-$MANIFEST_REPO/build}"
ENV_JSON="${NCS_ENV_JSON:-}"

usage() {
    cat <<EOF
Usage: build-rak4630.sh <action>

Actions:
  bootstrap  Initialize the west workspace in vendor/ and run west update
  patch      Apply vendor west patches in the manifest repo
  configure  Run a pristine west build for the ButterFi-integrated app
  build      Run incremental ninja build in the existing build directory
  rebuild    Run bootstrap, patch, and configure in sequence
  help       Show this message

Environment overrides:
  NCS_ENV_JSON            Path to toolchain environment.json
  NCS_TOOLCHAIN_DIR       Toolchain root containing environment.json
  BUTTERFI_VENDOR_ROOT    West workspace root (default: REPO_ROOT/vendor)
  BUTTERFI_MANIFEST_REPO  Manifest repo path (default: VENDOR_ROOT/RAK4630-Amazon-Sidewalk-Example)
  BUTTERFI_BOARD          West board name (default: rak4631)
  BUTTERFI_APP            App path inside manifest repo (default: app/rak4631_rak1901_demo)
  BUTTERFI_OVERLAY        Overlay config filename (default: lora.conf)
  BUTTERFI_BUILD_DIR      Build output directory (default: MANIFEST_REPO/build)
  BUTTERFI_WEST_UPDATE_ARGS  Extra arguments appended to west update

Examples:
  NCS_ENV_JSON=/path/to/environment.json ./scripts/build-rak4630.sh rebuild
  ./scripts/build-rak4630.sh build
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
    if [[ ! -d "$VENDOR_ROOT" ]]; then
        echo "Vendor workspace root not found: $VENDOR_ROOT" >&2
        exit 1
    fi

    if [[ ! -d "$MANIFEST_REPO" ]]; then
        echo "Manifest repo not found: $MANIFEST_REPO" >&2
        exit 1
    fi
}

bootstrap() {
    local -a update_args=()

    require_paths
    if [[ ! -d "$VENDOR_ROOT/.west" ]]; then
        (
            cd "$VENDOR_ROOT"
            run_cmd west init -l RAK4630-Amazon-Sidewalk-Example
        )
    fi

    if [[ -n "${BUTTERFI_WEST_UPDATE_ARGS:-}" ]]; then
        # Intentional word splitting so callers can pass multiple west flags.
        read -r -a update_args <<<"${BUTTERFI_WEST_UPDATE_ARGS}"
    fi

    (
        cd "$VENDOR_ROOT"
        run_cmd west update "${update_args[@]}"
    )
}

patch_workspace() {
    require_paths
    (
        cd "$MANIFEST_REPO"
        run_cmd west patch -a
    )
}

configure_build() {
    require_paths
    (
        cd "$MANIFEST_REPO"
        run_cmd west build -p -b "$BOARD" "$APP" -- "-DOVERLAY_CONFIG=$OVERLAY"
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
    bootstrap)
        bootstrap
        ;;
    patch)
        patch_workspace
        ;;
    configure)
        configure_build
        ;;
    build)
        incremental_build
        ;;
    rebuild)
        bootstrap
        patch_workspace
        configure_build
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