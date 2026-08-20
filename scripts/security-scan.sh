#!/usr/bin/env bash
#
# ButterFi static security + correctness checks.
#
# Runs the SAME tools locally and in CI (.github/workflows/security.yml calls
# this script), so the two can never drift:
#
#   - cfn-lint : CloudFormation correctness/validity            (hard gate)
#   - checkov  : CloudFormation security/misconfig, per         (hard gate)
#                .checkov.yaml (accepted checks are skipped there)
#   - bandit   : Python SAST on the inline Lambdas + scripts/   (gate on HIGH;
#                MEDIUM/LOW are reported as advisory)
#
# The inline Lambda code lives in template.yaml `Code.ZipFile:` blocks, which the
# IaC scanners treat as opaque strings — so it is extracted to temp .py files for
# bandit (that is the file with the SSRF-class urlopen surface).
#
# Local:  ./scripts/security-scan.sh
# Install the tools with: pip install -r scripts/security-requirements.txt
#
# Exit code is the gate: non-zero if any hard check fails.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

TEMPLATE="template.yaml"
fail=0

hr() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: '$1' not found. Install with: pip install -r scripts/security-requirements.txt" >&2
        exit 3
    fi
}

require cfn-lint
require checkov
require bandit
require python3

# 1. CloudFormation correctness -------------------------------------------------
hr "cfn-lint — CloudFormation correctness"
if cfn-lint "$TEMPLATE"; then
    echo "cfn-lint: OK"
else
    echo "cfn-lint: FAILED"
    fail=1
fi

# 2. CloudFormation security ----------------------------------------------------
hr "checkov — CloudFormation security"
if checkov -f "$TEMPLATE" --config-file .checkov.yaml; then
    echo "checkov: OK"
else
    echo "checkov: FAILED (a check outside .checkov.yaml's accepted list)"
    fail=1
fi

# 3. Python SAST on the inline Lambdas + scripts --------------------------------
hr "bandit — Python SAST (inline Lambdas + scripts/)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 scripts/extract-inline-lambdas.py "$tmp" >/dev/null

echo "--- findings (MEDIUM+; advisory) ---"
bandit -r "$tmp" scripts/ -ll -q -f custom \
    --msg-template "{severity}/{confidence} {test_id} {relpath}:{line} {msg}" 2>/dev/null || true

echo "--- gate (HIGH severity blocks) ---"
if bandit -r "$tmp" scripts/ -lll -q >/dev/null 2>&1; then
    echo "bandit: OK (no HIGH-severity findings)"
else
    echo "bandit: FAILED (HIGH-severity finding)"
    fail=1
fi

# Summary -----------------------------------------------------------------------
hr "result"
if [ "$fail" -eq 0 ]; then
    echo "PASS — all hard checks clean. (Review any advisory bandit/checkov notes above.)"
else
    echo "FAIL — see the failed check(s) above."
fi
exit "$fail"
