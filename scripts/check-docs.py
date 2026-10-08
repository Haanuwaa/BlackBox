#!/usr/bin/env python3
"""Check repository documentation destinations and GitHub-style heading anchors.

Offline by design: external URLs and ignored development artifacts are not fetched.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path
from urllib.parse import unquote, urlsplit


def prose(text: str) -> str:
    """Ignore fenced examples while preserving line numbers."""
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            if fence is None:
                fence = marker[1][0]
            elif marker[1][0] == fence:
                fence = None
            lines.append("")
        else:
            lines.append(line if fence is None else "")
    return "\n".join(lines)


def headings(text: str) -> set[str]:
    result: set[str] = set()
    counts: dict[str, int] = {}
    for heading in re.findall(r"^#{1,6}\s+(.+?)\s*#*\s*$", prose(text), re.M):
        heading = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", heading)
        base = re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")
        index = counts.get(base, 0)
        counts[base] = index + 1
        result.add(base + (f"-{index}" if index else ""))
    result.update(re.findall(r'<a\s+(?:id|name)=["\']([^"\']+)', text))
    return result


def check(root: Path) -> list[str]:
    errors = []
    files = sorted(set(root.glob("*.md")) | set((root / "docs").rglob("*.md")))
    anchors = {path.resolve(): headings(path.read_text(encoding="utf-8")) for path in files}
    for path in files:
        for number, line in enumerate(prose(path.read_text(encoding="utf-8")).splitlines(), 1):
            destinations = re.findall(r"\[[^\]]*\]\(([^)]+)\)", line)
            destinations += re.findall(r"^\s*\[[^\]]+\]:\s*(\S+)", line)
            for destination in destinations:
                destination = destination.split(' "', 1)[0].strip("<>")
                parsed = urlsplit(destination)
                if parsed.scheme or parsed.netloc:
                    continue
                relative = unquote(parsed.path)
                target = ((root if relative.startswith("/") else path.parent) / relative.lstrip("/")).resolve() if relative else path.resolve()
                if not target.is_relative_to(root.resolve()):
                    errors.append(f"{path.relative_to(root)}:{number}: link escapes repository: {destination}")
                elif not target.exists():
                    errors.append(f"{path.relative_to(root)}:{number}: missing target: {destination}")
                elif parsed.fragment and target.suffix.lower() == ".md":
                    known = anchors.get(target)
                    if known is None:
                        known = headings(target.read_text(encoding="utf-8"))
                        anchors[target] = known
                    if unquote(parsed.fragment) not in known:
                        errors.append(f"{path.relative_to(root)}:{number}: missing heading: {destination}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    errors = check(args.root.resolve())
    for error in errors:
        print(error)
    if not errors:
        print("Documentation links and heading anchors verified")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
