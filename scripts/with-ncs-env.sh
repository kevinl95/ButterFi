#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: with-ncs-env.sh [--env-json PATH] -- command [args...]

Runs a command with environment variables loaded from a Nordic toolchain
environment.json file. If no environment file is provided, the command is
executed unchanged.

Environment fallbacks:
  NCS_ENV_JSON       Full path to environment.json
  NCS_TOOLCHAIN_DIR  Toolchain root containing environment.json
EOF
}

env_json="${NCS_ENV_JSON:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --env-json)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --env-json" >&2
                exit 2
            fi
            env_json="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ $# -eq 0 ]]; then
    usage >&2
    exit 2
fi

if [[ -z "$env_json" && -n "${NCS_TOOLCHAIN_DIR:-}" ]]; then
    env_json="${NCS_TOOLCHAIN_DIR%/}/environment.json"
fi

if [[ -n "$env_json" ]]; then
    if [[ ! -f "$env_json" ]]; then
        echo "NCS environment file not found: $env_json" >&2
        exit 1
    fi

    eval "$({
        python3 - "$env_json" <<'PY'
import json
import os
import shlex
import sys

env_json = os.path.abspath(sys.argv[1])
toolchain_root = os.path.dirname(env_json)

with open(env_json, "r", encoding="utf-8") as handle:
    data = json.load(handle)

for spec in data.get("env_vars", []):
    key = spec["key"]
    spec_type = spec["type"]

    if spec_type == "string":
        value = spec["value"]
    elif spec_type == "relative_paths":
        pieces = [os.path.join(toolchain_root, rel_path) for rel_path in spec.get("values", [])]
        joined = os.pathsep.join(pieces)
        treatment = spec.get("existing_value_treatment", "overwrite")
        existing = os.environ.get(key, "")
        if treatment == "prepend_to" and existing:
            value = joined + os.pathsep + existing
        elif treatment == "append_to" and existing:
            value = existing + os.pathsep + joined
        else:
            value = joined
    else:
        continue

    print(f"export {key}={shlex.quote(value)}")
PY
    })"
fi

exec "$@"