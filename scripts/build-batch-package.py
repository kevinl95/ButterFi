#!/usr/bin/env python3

"""Build a butterfi-batch-v1 package for the browser provisioner.

This script is intended for admin or backend-side packaging, not for classroom
operators. It embeds one shared UF2 plus one credential file per device into a
single JSON bundle consumed by web/provision.html.
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
from pathlib import Path
from typing import Any


def infer_format(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".uf2":
        return "uf2"
    if suffix == ".hex":
        return "hex"
    if suffix == ".json":
        return "json"
    return "bin"


def encode_asset(path: Path) -> dict[str, str]:
    return {
        "file_name": path.name,
        "format": infer_format(path),
        "content_base64": base64.b64encode(path.read_bytes()).decode("ascii"),
    }


def resolve_path(base_dir: Path, value: str) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = (base_dir / candidate).resolve()
    if not candidate.exists():
        raise FileNotFoundError(f"missing file: {candidate}")
    return candidate


def load_manifest_rows(manifest_path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    if manifest_path.suffix.lower() == ".csv":
        with manifest_path.open("r", encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))
        return {}, rows

    parsed = json.loads(manifest_path.read_text(encoding="utf-8"))
    if isinstance(parsed, list):
        return {}, parsed
    if isinstance(parsed, dict):
        devices = parsed.get("devices")
        if not isinstance(devices, list):
            raise ValueError("JSON manifest must contain a devices array")
        defaults = parsed.get("defaults")
        return defaults if isinstance(defaults, dict) else {}, devices
    raise ValueError("manifest must be CSV, a JSON array, or a JSON object with devices")


def normalize_device(
    row: dict[str, Any],
    base_dir: Path,
    default_school_id: str | None,
    default_content_pkg: str | None,
) -> dict[str, Any]:
    device_id = str(row.get("device_id") or row.get("deviceId") or "").strip()
    if not device_id:
        raise ValueError("each device row must include device_id")

    credential_path_value = row.get("credential_path") or row.get("credentialPath")
    if not credential_path_value:
        raise ValueError(f"device {device_id} is missing credential_path")

    credential_path = resolve_path(base_dir, str(credential_path_value))
    device_name = str(row.get("device_name") or row.get("deviceName") or row.get("label") or device_id).strip()
    school_id = str(row.get("school_id") or row.get("schoolId") or "").strip()
    content_pkg = str(row.get("content_pkg") or row.get("contentPkg") or "").strip()

    device: dict[str, Any] = {
        "device_id": device_id,
        "device_name": device_name,
        "credential": encode_asset(credential_path),
    }

    if school_id and school_id != (default_school_id or ""):
        device["school_id"] = school_id
    if content_pkg and content_pkg != (default_content_pkg or ""):
        device["content_pkg"] = content_pkg

    return device


def build_package(args: argparse.Namespace) -> dict[str, Any]:
    manifest_path = Path(args.manifest).resolve()
    uf2_path = Path(args.uf2).resolve()
    output_path = Path(args.output).resolve()

    if not manifest_path.exists():
        raise FileNotFoundError(f"missing manifest: {manifest_path}")
    if not uf2_path.exists():
        raise FileNotFoundError(f"missing uf2: {uf2_path}")

    manifest_defaults, rows = load_manifest_rows(manifest_path)
    base_dir = manifest_path.parent

    default_school_id = args.default_school_id or manifest_defaults.get("school_id") or manifest_defaults.get("schoolId")
    default_content_pkg = args.default_content_pkg or manifest_defaults.get("content_pkg") or manifest_defaults.get("contentPkg")

    seen_device_ids: set[str] = set()
    devices = []
    for row in rows:
        device = normalize_device(row, base_dir, default_school_id, default_content_pkg)
        if device["device_id"] in seen_device_ids:
            raise ValueError(f"duplicate device_id: {device['device_id']}")
        seen_device_ids.add(device["device_id"])
        devices.append(device)

    if not devices:
        raise ValueError("manifest did not produce any devices")

    package: dict[str, Any] = {
        "format": "butterfi-batch-v1",
        "batch_id": args.batch_id,
        "label": args.label or args.batch_id,
        "firmware": encode_asset(uf2_path),
        "devices": devices,
    }

    defaults: dict[str, str] = {}
    if default_school_id:
        defaults["school_id"] = str(default_school_id)
    if default_content_pkg:
        defaults["content_pkg"] = str(default_content_pkg)
    if defaults:
        package["defaults"] = defaults

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(package, indent=2) + "\n", encoding="utf-8")
    return package


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a butterfi-batch-v1 JSON package")
    parser.add_argument("--uf2", required=True, help="Path to the shared ButterFi UF2")
    parser.add_argument(
        "--manifest",
        required=True,
        help=(
            "Path to device manifest (.csv or .json) with device_id, device_name, "
            "school_id, content_pkg, credential_path. credential_path should point "
            "to a raw certificate.json or a .hex/.bin credential asset, such as the "
            "output of scripts/build-sidewalk-credential.py"
        ),
    )
    parser.add_argument("--output", required=True, help="Path to write the batch JSON")
    parser.add_argument("--batch-id", required=True, help="Stable batch identifier used for browser-side progress tracking")
    parser.add_argument("--label", help="Operator-facing label shown in the browser")
    parser.add_argument("--default-school-id", help="Default school_id for devices that omit it")
    parser.add_argument("--default-content-pkg", help="Default content_pkg for devices that omit it")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    package = build_package(args)
    print(
        f"wrote {args.output} with {len(package['devices'])} devices "
        f"for batch {package['batch_id']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())