#!/usr/bin/env python3
"""Regenerate the generated sections of docs/COMMAND_DIALECT.md from the legacy tree.

Sources (read-only): pixil_utils/parameter_types.py PARAMETER_TYPES (script-side
signatures) and rgb_matrix_lib/commands.py command_handlers (consumer-side verbs).

Usage: tools/gen_command_dialect.py [--legacy-root DIR] [--check]
"""

from __future__ import annotations

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(REPO, "docs", "COMMAND_DIALECT.md")
BEGIN = "<!-- BEGIN GENERATED: {} -->"
END = "<!-- END GENERATED: {} -->"


def signatures(legacy: str) -> str:
    sys.path.insert(0, legacy)
    from pixil_utils.parameter_types import PARAMETER_TYPES  # noqa: E402

    lines = ["| Command | Parameters (`?` optional, `=default`) |", "|---|---|"]
    for cmd in sorted(PARAMETER_TYPES):
        parts = []
        for p in sorted(PARAMETER_TYPES[cmd], key=lambda p: p["position"]):
            s = f"{p['name']}:{p['type']}"
            if p.get("optional"):
                s += "?"
            if "default" in p:
                s += f"={p['default']}"
            parts.append(f"`{s}`")
        lines.append(f"| `{cmd}` | {', '.join(parts) if parts else '—'} |")
    return "\n".join(lines)


def consumer_verbs(legacy: str) -> str:
    src = open(os.path.join(legacy, "rgb_matrix_lib", "commands.py"), encoding="utf-8").read()
    verbs = sorted(set(re.findall(r"'(\w+)':\s*self\._handle_\w+", src)))
    return "`" + "`, `".join(verbs) + "`" + f"\n\n({len(verbs)} verbs)"


def replace(doc: str, key: str, body: str) -> str:
    b, e = BEGIN.format(key), END.format(key)
    pattern = re.compile(re.escape(b) + r".*?" + re.escape(e), re.S)
    if not pattern.search(doc):
        raise SystemExit(f"marker {b} missing in {DOC}")
    return pattern.sub(lambda _m: f"{b}\n{body}\n{e}", doc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--legacy-root", default=os.environ.get("PIXIL_LEGACY_ROOT", os.path.join(REPO, "..", "git")))
    ap.add_argument("--check", action="store_true", help="exit 1 if the doc is out of date")
    args = ap.parse_args()
    legacy = os.path.abspath(args.legacy_root)

    doc = open(DOC, encoding="utf-8").read()
    new = replace(doc, "signatures", signatures(legacy))
    new = replace(new, "consumer-verbs", consumer_verbs(legacy))
    if args.check:
        if new != doc:
            print("docs/COMMAND_DIALECT.md is out of date; run tools/gen_command_dialect.py", file=sys.stderr)
            return 1
        return 0
    if new != doc:
        open(DOC, "w", encoding="utf-8").write(new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
