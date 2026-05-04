#!/usr/bin/env python3

"""Build ButterFi-ready Sidewalk credential assets from AWS exports.

This is an admin-side wrapper around the official Sidewalk SDK provisioner.
It converts either:

- a combined AWS IoT Wireless `certificate.json`, or
- a `get-wireless-device` JSON plus `get-device-profile` JSON

into `.bin` and `.hex` manufacturing-page assets targeted at the ButterFi XIAO
`mfg_storage` partition. The generated files can be uploaded directly in
`web/provision.html` or embedded into a `butterfi-batch-v1` package.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PM_STATIC = REPO_ROOT / "firmware" / "xiao_nrf52840" / "pm_static.yml"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "artifacts" / "sidewalk-credentials"
DEFAULT_CHIP = "nrf52840"


def parse_int(value: str) -> int:
    return int(value, 0)


def resolve_file(path_value: str, label: str) -> Path:
    path = Path(path_value).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def resolve_sidewalk_base(path_value: str | None) -> Path:
    candidates: list[Path] = []
    if path_value:
        candidates.append(Path(path_value).expanduser())

    env_value = os.environ.get("SIDEWALK_BASE")
    if env_value:
        env_path = Path(env_value).expanduser()
        if env_path not in candidates:
            candidates.append(env_path)

    for candidate in candidates:
        if (candidate / "tools" / "provision" / "provision.py").is_file():
            return candidate.resolve()

    searched = ", ".join(str(candidate) for candidate in candidates) or "SIDEWALK_BASE"
    raise FileNotFoundError(
        "Could not find the Sidewalk provisioner. "
        f"Checked: {searched}. Set --sidewalk-base or SIDEWALK_BASE to your sdk-sidewalk checkout."
    )


def read_partition_address(pm_static_path: Path, partition_name: str = "mfg_storage") -> int:
    in_partition = False
    for raw_line in pm_static_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip()
        stripped = line.strip()

        if not stripped or stripped.startswith("#"):
            continue

        if not in_partition:
            if stripped == f"{partition_name}:":
                in_partition = True
            continue

        if raw_line[:1] not in {" ", "\t"}:
            break

        key, _, value = stripped.partition(":")
        if key == "address" and value:
            return int(value.strip(), 0)

    raise ValueError(f"Could not find {partition_name}.address in {pm_static_path}")


def validate_basename(value: str) -> str:
    basename = value.strip()
    if not basename:
        raise ValueError("basename must not be empty")
    if Path(basename).name != basename:
        raise ValueError("basename must be a file stem, not a path")
    if basename in {".", ".."}:
        raise ValueError("basename must be a normal file stem")
    return basename


def derive_basename(args: argparse.Namespace) -> str:
    if args.basename:
        return validate_basename(args.basename)

    if args.certificate_json:
        stem = Path(args.certificate_json).stem
    else:
        stem = Path(args.wireless_device_json).stem

    if stem.lower() in {"certificate", "wireless_device", "device_profile"}:
        return "sidewalk-credential"
    return validate_basename(stem)


def build_command(
    args: argparse.Namespace,
    provision_tool: Path,
    output_bin: Path,
    output_hex: Path,
    addr: int,
) -> list[str]:
    command = [
        sys.executable,
        str(provision_tool),
        "nordic",
        "aws",
        "--chip",
        args.chip,
        "--addr",
        hex(addr),
        "--output_bin",
        str(output_bin),
        "--output_hex",
        str(output_hex),
    ]

    if args.dump_raw_values:
        command.append("--dump_raw_values")

    if args.certificate_json:
        command.extend(["--certificate_json", str(resolve_file(args.certificate_json, "certificate_json"))])
    else:
        command.extend(
            [
                "--wireless_device_json",
                str(resolve_file(args.wireless_device_json, "wireless_device_json")),
                "--device_profile_json",
                str(resolve_file(args.device_profile_json, "device_profile_json")),
            ]
        )

    return command


def format_subprocess_failure(result: subprocess.CompletedProcess[str], requirements_path: Path) -> str:
    combined = "\n".join(part for part in [result.stdout, result.stderr] if part).strip()
    lines = ["Sidewalk credential conversion failed."]

    if "ModuleNotFoundError" in combined and ("yaml" in combined or "intelhex" in combined):
        lines.append(
            "Install the official Sidewalk provisioner dependencies in the Python environment "
            f"you are using for this script: {sys.executable} -m pip install -r {requirements_path}"
        )

    if combined:
        lines.append("Provisioner output:")
        lines.append(combined)

    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert AWS Sidewalk provisioning exports into ButterFi XIAO "
            "credential .bin/.hex files for the browser provisioner."
        )
    )
    parser.add_argument(
        "--certificate-json",
        help="Combined AWS IoT Wireless certificate.json downloaded from the AWS console.",
    )
    parser.add_argument(
        "--wireless-device-json",
        help="JSON saved from aws iotwireless get-wireless-device.",
    )
    parser.add_argument(
        "--device-profile-json",
        help="JSON saved from aws iotwireless get-device-profile.",
    )
    parser.add_argument(
        "--sidewalk-base",
        default=os.environ.get("SIDEWALK_BASE"),
        help="Path to the sdk-sidewalk checkout. Defaults to SIDEWALK_BASE.",
    )
    parser.add_argument(
        "--pm-static",
        default=str(DEFAULT_PM_STATIC),
        help="Path to the ButterFi partition map used to derive the mfg_storage address.",
    )
    parser.add_argument(
        "--addr",
        type=parse_int,
        help="Override the mfg_storage address. Defaults to the address in pm_static.yml.",
    )
    parser.add_argument(
        "--chip",
        default=DEFAULT_CHIP,
        help="Nordic chip name to pass through to the official provisioner.",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="Directory where the generated .bin and .hex files will be written.",
    )
    parser.add_argument(
        "--basename",
        help="Output file stem. Defaults to the input filename stem.",
    )
    parser.add_argument(
        "--dump-raw-values",
        action="store_true",
        help="Pass through the SDK provisioner's raw-value debug dump.",
    )
    args = parser.parse_args()

    using_certificate = bool(args.certificate_json)
    using_pair = bool(args.wireless_device_json or args.device_profile_json)

    if using_certificate and using_pair:
        parser.error(
            "Provide either --certificate-json or the pair --wireless-device-json/--device-profile-json, not both"
        )

    if not using_certificate and not using_pair:
        parser.error(
            "Provide either --certificate-json or both --wireless-device-json and --device-profile-json"
        )

    if using_pair and not (args.wireless_device_json and args.device_profile_json):
        parser.error("Both --wireless-device-json and --device-profile-json are required together")

    return args


def main() -> int:
    args = parse_args()
    sidewalk_base = resolve_sidewalk_base(args.sidewalk_base)
    provision_tool = sidewalk_base / "tools" / "provision" / "provision.py"
    requirements_path = sidewalk_base / "tools" / "provision" / "requirements.txt"

    pm_static_path = resolve_file(args.pm_static, "pm_static")
    addr = args.addr if args.addr is not None else read_partition_address(pm_static_path)

    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    basename = derive_basename(args)
    output_bin = output_dir / f"{basename}.bin"
    output_hex = output_dir / f"{basename}.hex"

    command = build_command(args, provision_tool, output_bin, output_hex, addr)
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(format_subprocess_failure(result, requirements_path))

    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)

    print(f"ButterFi XIAO mfg_storage address: {hex(addr)}")
    print(f"Generated {output_bin}")
    print(f"Generated {output_hex}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())