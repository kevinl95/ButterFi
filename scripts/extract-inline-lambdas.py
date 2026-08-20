#!/usr/bin/env python3

"""Extract the inline Lambda code from template.yaml into standalone .py files.

The ButterFi Lambdas live inline in CloudFormation `Code.ZipFile:` blocks, which
IaC scanners treat as opaque strings. This dumps each block to <out_dir>/<Name>.py
so a Python SAST (bandit/semgrep) can scan the actual handler code. Used by
scripts/security-scan.sh; safe to run standalone.

Usage: extract-inline-lambdas.py <out_dir> [template.yaml]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

BLOCK_INDENT = 10  # ZipFile: | content is indented 10 spaces in template.yaml


def extract(template_path: Path, out_dir: Path) -> list[Path]:
    lines = template_path.read_text(encoding="utf-8").split("\n")
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    resource = None
    i = 0
    while i < len(lines):
        line = lines[i]
        header = re.match(r"^  ([A-Za-z0-9]+):\s*$", line)  # resource at 2-space indent
        if header:
            resource = header.group(1)
        if line.strip() == "ZipFile: |":
            body: list[str] = []
            j = i + 1
            while j < len(lines):
                ln = lines[j]
                if ln.strip() == "":
                    body.append("")
                    j += 1
                    continue
                if len(ln) - len(ln.lstrip(" ")) < BLOCK_INDENT:
                    break
                body.append(ln[BLOCK_INDENT:])
                j += 1
            while body and body[-1] == "":
                body.pop()
            name = resource or f"lambda_{len(written)}"
            path = out_dir / f"{name}.py"
            path.write_text("\n".join(body) + "\n", encoding="utf-8")
            written.append(path)
            i = j
            continue
        i += 1
    return written


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out_dir = Path(sys.argv[1])
    template_path = Path(sys.argv[2]) if len(sys.argv) > 2 else (Path(__file__).resolve().parents[1] / "template.yaml")
    written = extract(template_path, out_dir)
    for path in written:
        print(path)
    print(f"extracted {len(written)} inline Lambda(s) from {template_path.name}", file=sys.stderr)
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
