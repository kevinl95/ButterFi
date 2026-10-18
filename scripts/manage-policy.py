#!/usr/bin/env python3

"""Manage the ButterFi content policy — a per-school allowlist / blocklist.

The scraper enforces this server-side:
  - a domain on the BLOCK list is always denied (block wins);
  - if the ALLOW list is non-empty, only domains on it (and their subdomains)
    are reachable (allow-only mode);
  - an empty policy = open web (the default).

Matching is by registrable domain and covers subdomains: blocking "example.com"
also blocks "www.example.com" and "ads.example.com". Search still works in
allow-only mode; a search result is checked when the student opens it.

Examples:
  # block a few trackers/ad domains
  scripts/manage-policy.py block add ads.example doubleclick.net

  # lock browsing down to an allowlist
  scripts/manage-policy.py allow add wikipedia.org khanacademy.org weather.gov

  # inspect the current lists
  scripts/manage-policy.py allow list
  scripts/manage-policy.py block list

  # remove an entry
  scripts/manage-policy.py block remove ads.example

The policy table is resolved from the stack's PolicyTableName output
(--stack-name, default "butterfi") unless you pass --table. The stack lives in
us-east-1. Changes take effect within ~60s (the scraper caches the policy).
"""

from __future__ import annotations

import argparse
import sys
from urllib.parse import urlparse

import boto3
from boto3.dynamodb.conditions import Key

LIST_TYPES = {"allow": "ALLOW", "block": "BLOCK"}


def normalize_domain(value: str) -> str:
    """Reduce user input to a bare host: 'https://www.X.com/a' / 'X.com/a' -> host."""
    v = value.strip().lower()
    if not v:
        return ""
    if "://" not in v:
        v = "//" + v  # let urlparse extract the host from "example.com/path"
    return (urlparse(v).hostname or "").rstrip(".")


def resolve_table_name(args: argparse.Namespace) -> str:
    if args.table:
        return args.table
    cf = boto3.client("cloudformation", region_name=args.region)
    stacks = cf.describe_stacks(StackName=args.stack_name)["Stacks"]
    for output in stacks[0].get("Outputs", []):
        if output["OutputKey"] == "PolicyTableName":
            return output["OutputValue"]
    raise SystemExit(
        f"stack {args.stack_name!r} has no PolicyTableName output — pass --table explicitly"
    )


def query_list(table, list_type: str) -> list[str]:
    domains: list[str] = []
    resp = table.query(KeyConditionExpression=Key("listType").eq(list_type))
    domains += [item["domain"] for item in resp.get("Items", [])]
    while "LastEvaluatedKey" in resp:
        resp = table.query(
            KeyConditionExpression=Key("listType").eq(list_type),
            ExclusiveStartKey=resp["LastEvaluatedKey"],
        )
        domains += [item["domain"] for item in resp.get("Items", [])]
    return sorted(domains)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("list_type", choices=sorted(LIST_TYPES), help="which list to act on")
    p.add_argument("action", choices=["add", "remove", "list"], help="what to do")
    p.add_argument("domains", nargs="*", help="domains, for add/remove")
    p.add_argument("--table", help="policy table name (else resolved from the stack)")
    p.add_argument("--stack-name", default="butterfi", help="CloudFormation stack (default: butterfi)")
    p.add_argument("--region", default="us-east-1", help="AWS region (default: us-east-1)")
    args = p.parse_args()
    if args.action in ("add", "remove") and not args.domains:
        p.error(f"{args.action} needs at least one domain")
    return args


def main() -> int:
    args = parse_args()
    list_type = LIST_TYPES[args.list_type]
    table_name = resolve_table_name(args)
    table = boto3.resource("dynamodb", region_name=args.region).Table(table_name)

    if args.action == "list":
        domains = query_list(table, list_type)
        if domains:
            print(f"{list_type} list ({len(domains)} domain(s)):")
            for d in domains:
                print(f"  {d}")
        else:
            print(f"{list_type} list is empty.")
        return 0

    changed = 0
    for raw in args.domains:
        domain = normalize_domain(raw)
        if not domain:
            print(f"  skip (no host in {raw!r})", file=sys.stderr)
            continue
        if args.action == "add":
            table.put_item(Item={"listType": list_type, "domain": domain})
            print(f"  + {list_type}  {domain}")
        else:
            table.delete_item(Key={"listType": list_type, "domain": domain})
            print(f"  - {list_type}  {domain}")
        changed += 1

    print(f"\n{changed} change(s) to the {list_type} list ({table_name}).")
    print("Takes effect within ~60s (the scraper caches the policy).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
