#!/usr/bin/env python3

"""Bulk-provision ButterFi Sidewalk devices from a school's AWS account.

Closes the turnkey gap for a 30-device rollout: given one Sidewalk device
profile, it loops the per-device AWS work and emits a batch manifest ready for
web/provision.html.

For each device it:
  1. `aws iotwireless create-wireless-device` (Sidewalk) under the profile,
     bound to the ButterFi destination,
  2. `aws iotwireless get-wireless-device` to export its credential material,
  3. runs scripts/build-sidewalk-credential.py to produce the mfg_storage
     .hex/.bin, and
  4. appends a row to a CSV manifest.

Finally (with --uf2) it can invoke scripts/build-batch-package.py to bundle a
single `butterfi-batch-v1` file the classroom operator loads once.

SAFETY: creating wireless devices is a real, billable AWS mutation, so this is
DRY-RUN by default — it prints the plan and makes no changes. Add --execute to
actually create devices. Re-running with --execute RESUMES: any device whose
credential already exists in the output dir is skipped, so a mid-run failure is
safe to re-run.

Prerequisites:
  - `aws` CLI configured for the school's account (Sidewalk lives in us-east-1).
  - The official Sidewalk provisioner reachable via SIDEWALK_BASE (or
    --sidewalk-base), same as build-sidewalk-credential.py.

Examples:
  # Preview 30 devices for Room 204 (no changes):
  python3 scripts/bulk-provision-aws.py \\
    --device-profile-id c5f317ac-fe2e-4aa1-8031-e87ffb11d04a \\
    --count 30 --name-prefix ROOM-204 \\
    --school-id BOULDER-HS-01 --content-pkg k12-general \\
    --batch-id room-204-spring-2026

  # Actually create them and bundle the batch file:
  python3 scripts/bulk-provision-aws.py \\
    --device-profile-id c5f317ac-fe2e-4aa1-8031-e87ffb11d04a \\
    --count 30 --name-prefix ROOM-204 \\
    --school-id BOULDER-HS-01 --content-pkg k12-general \\
    --batch-id room-204-spring-2026 \\
    --uf2 artifacts/butterfi-xiao-sidewalk.uf2 \\
    --execute
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CREDENTIAL_BUILDER = REPO_ROOT / "scripts" / "build-sidewalk-credential.py"
BATCH_BUILDER = REPO_ROOT / "scripts" / "build-batch-package.py"
DEFAULT_OUTPUT_ROOT = REPO_ROOT / "provisioning"  # gitignored


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def run(cmd: list[str], *, capture: bool = True) -> str:
    """Run a subprocess, raising a readable error on failure."""
    result = subprocess.run(
        cmd,
        capture_output=capture,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(cmd)}\n{detail}")
    return result.stdout if capture else ""


def aws_json(args: list[str], region: str) -> dict:
    out = run(["aws", "iotwireless", *args, "--region", region, "--output", "json"])
    return json.loads(out) if out.strip() else {}


def build_device_list(args: argparse.Namespace) -> list[dict[str, str]]:
    """Resolve the (device_id, device_name) list from --count/--name-prefix or
    a --names-file (one 'device_id[,device_name]' per line)."""
    devices: list[dict[str, str]] = []
    if args.names_file:
        for raw in Path(args.names_file).read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            device_id, _, name = line.partition(",")
            device_id = device_id.strip()
            devices.append({"device_id": device_id, "device_name": (name.strip() or device_id)})
    else:
        for n in range(args.start_index, args.start_index + args.count):
            device_id = f"{args.name_prefix}-{n:03d}"
            devices.append({"device_id": device_id, "device_name": f"{args.name_prefix} / {n:03d}"})
    seen = set()
    for d in devices:
        if not d["device_id"]:
            raise ValueError("empty device_id in device list")
        if d["device_id"] in seen:
            raise ValueError(f"duplicate device_id: {d['device_id']}")
        seen.add(d["device_id"])
    return devices


def provision_one(
    device: dict[str, str],
    *,
    profile_id: str,
    profile_json_path: Path,
    destination: str,
    region: str,
    aws_dir: Path,
    cred_dir: Path,
    sidewalk_base: str | None,
) -> Path:
    """Create + export + build credential for one device. Returns the .hex path.
    Resumes: if the .hex already exists, no AWS call is made."""
    device_id = device["device_id"]
    hex_path = cred_dir / f"{device_id}.hex"
    if hex_path.is_file():
        eprint(f"  [skip] {device_id}: credential already present")
        return hex_path

    # 1. create the Sidewalk wireless device under the profile
    created = aws_json(
        [
            "create-wireless-device",
            "--type", "Sidewalk",
            "--name", device["device_name"],
            "--destination-name", destination,
            "--sidewalk", f"DeviceProfileId={profile_id}",
        ],
        region,
    )
    wireless_id = created["Id"]

    # 2. export the device's credential material
    wd = aws_json(
        ["get-wireless-device", "--identifier", wireless_id, "--identifier-type", "WirelessDeviceId"],
        region,
    )
    dev_aws_dir = aws_dir / device_id
    dev_aws_dir.mkdir(parents=True, exist_ok=True)
    wd_path = dev_aws_dir / "wireless_device.json"
    wd_path.write_text(json.dumps(wd, indent=2), encoding="utf-8")
    (dev_aws_dir / "wireless_device_id.txt").write_text(wireless_id + "\n", encoding="utf-8")

    # 3. build the mfg_storage credential (.hex/.bin) via the official provisioner
    cmd = [
        sys.executable, str(CREDENTIAL_BUILDER),
        "--wireless-device-json", str(wd_path),
        "--device-profile-json", str(profile_json_path),
        "--basename", device_id,
        "--output-dir", str(cred_dir),
    ]
    if sidewalk_base:
        cmd += ["--sidewalk-base", sidewalk_base]
    run(cmd)
    if not hex_path.is_file():
        raise RuntimeError(f"{device_id}: credential builder did not produce {hex_path}")
    eprint(f"  [ok]   {device_id}: WirelessDeviceId={wireless_id}")
    return hex_path


def write_manifest(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["device_id", "device_name", "credential_path", "school_id", "content_pkg"],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device-profile-id", required=True, help="Sidewalk device profile ID (from the school's account)")
    p.add_argument("--destination-name", default="ButterFiDestination", help="Sidewalk destination (matches the stack)")
    p.add_argument("--region", default="us-east-1", help="AWS region (Sidewalk is us-east-1)")

    group = p.add_argument_group("device list (choose one)")
    group.add_argument("--count", type=int, help="number of devices to create")
    group.add_argument("--name-prefix", default="DEVICE", help="prefix for auto ids, e.g. ROOM-204 -> ROOM-204-001")
    group.add_argument("--start-index", type=int, default=1, help="first index for auto ids")
    group.add_argument("--names-file", help="file of 'device_id[,device_name]' lines instead of --count")

    p.add_argument("--school-id", default="", help="school/location id for the batch rows")
    p.add_argument("--content-pkg", default="", help="content package for the batch rows")
    p.add_argument("--batch-id", default="butterfi-batch", help="batch id (names the output dir + manifest)")
    p.add_argument("--batch-label", default="", help="operator-facing batch label")
    p.add_argument("--output-dir", help="output dir (default: provisioning/<batch-id>)")
    p.add_argument("--sidewalk-base", help="sdk-sidewalk checkout (else SIDEWALK_BASE); passed to the credential builder")
    p.add_argument("--uf2", help="if set, also invoke build-batch-package.py to produce the batch JSON")
    p.add_argument("--execute", action="store_true", help="actually create devices (default is a dry-run preview)")

    args = p.parse_args()
    if bool(args.count) == bool(args.names_file):
        p.error("provide exactly one of --count or --names-file")
    if args.count is not None and args.count <= 0:
        p.error("--count must be positive")
    # Fail fast on a bad --uf2 BEFORE creating any devices (a batch-step failure
    # after creation would otherwise leave orphan wireless devices).
    if args.uf2 and not Path(args.uf2).is_file():
        p.error(f"--uf2 not found: {args.uf2}")
    return args


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir) if args.output_dir else (DEFAULT_OUTPUT_ROOT / args.batch_id)
    aws_dir = output_dir / "aws"
    cred_dir = output_dir / "credentials"
    manifest_path = output_dir / f"{args.batch_id}-manifest.csv"

    devices = build_device_list(args)
    mode = "EXECUTE" if args.execute else "DRY-RUN (no changes; add --execute to create)"
    eprint(f"ButterFi bulk provision — {mode}")
    eprint(f"  profile      {args.device_profile_id}")
    eprint(f"  destination  {args.destination_name}  region {args.region}")
    eprint(f"  devices      {len(devices)}  ({devices[0]['device_id']} .. {devices[-1]['device_id']})")
    eprint(f"  output       {output_dir}")

    if not args.execute:
        eprint("\nWould create these Sidewalk wireless devices:")
        for d in devices:
            eprint(f"  - {d['device_id']:20} name={d['device_name']!r}")
        eprint("\nDry run only. Re-run with --execute to create them.")
        return 0

    # read-only: validate the profile exists and export it once
    output_dir.mkdir(parents=True, exist_ok=True)
    aws_dir.mkdir(parents=True, exist_ok=True)
    cred_dir.mkdir(parents=True, exist_ok=True)
    profile = aws_json(["get-device-profile", "--id", args.device_profile_id], args.region)
    profile_json_path = output_dir / "device_profile.json"
    profile_json_path.write_text(json.dumps(profile, indent=2), encoding="utf-8")

    rows: list[dict[str, str]] = []
    failures: list[tuple[str, str]] = []
    for device in devices:
        try:
            hex_path = provision_one(
                device,
                profile_id=args.device_profile_id,
                profile_json_path=profile_json_path,
                destination=args.destination_name,
                region=args.region,
                aws_dir=aws_dir,
                cred_dir=cred_dir,
                sidewalk_base=args.sidewalk_base,
            )
        except Exception as exc:  # continue the batch; report at the end
            eprint(f"  [FAIL] {device['device_id']}: {exc}")
            failures.append((device["device_id"], str(exc)))
            continue
        rows.append({
            "device_id": device["device_id"],
            "device_name": device["device_name"],
            "credential_path": str(hex_path),
            "school_id": args.school_id,
            "content_pkg": args.content_pkg,
        })

    if rows:
        write_manifest(manifest_path, rows)
        eprint(f"\nWrote manifest: {manifest_path}  ({len(rows)} devices)")

    if args.uf2 and rows:
        batch_out = output_dir / f"{args.batch_id}.json"
        cmd = [
            sys.executable, str(BATCH_BUILDER),
            "--uf2", args.uf2,
            "--manifest", str(manifest_path),
            "--output", str(batch_out),
            "--batch-id", args.batch_id,
        ]
        if args.batch_label:
            cmd += ["--label", args.batch_label]
        if args.school_id:
            cmd += ["--default-school-id", args.school_id]
        if args.content_pkg:
            cmd += ["--default-content-pkg", args.content_pkg]
        run(cmd, capture=False)
        eprint(f"Wrote batch package: {batch_out}")

    if failures:
        eprint(f"\n{len(failures)} device(s) FAILED (re-run to resume the rest):")
        for device_id, msg in failures:
            eprint(f"  - {device_id}: {msg.splitlines()[0]}")
        return 1
    eprint("\nAll devices provisioned. Load the batch package (or manifest) in web/provision.html.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
