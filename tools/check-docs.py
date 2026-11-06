#!/usr/bin/env python3
"""Check local Markdown targets and task identifiers; not teaching approval."""
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
errors = []
files = [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md"))]
links = 0
for path in files:
    content = path.read_text()
    if sum(line.startswith("```") for line in content.splitlines()) % 2:
        errors.append(f"{path.relative_to(ROOT)}: unclosed code fence")
    for target in re.findall(r"\]\(([^)]+)\)", content):
        target = target.strip("<>")
        if target.startswith(("https://", "http://", "mailto:", "#")):
            continue
        target = target.split("#", 1)[0]
        if not target:
            continue
        links += 1
        if not (path.parent / target).exists():
            errors.append(f"{path.relative_to(ROOT)}: missing target {target}")

backlog = (ROOT / "docs/plans/task-backlog.md").read_text()
tasks = re.findall(r"^- \[([ x])\] ([A-Z]+\d*-\d+) ", backlog, re.MULTILINE)
identifiers = [identifier for _, identifier in tasks]
if len(identifiers) != len(set(identifiers)):
    errors.append("duplicate task identifiers")
chapters = re.findall(r"^### (?:P\d{2}｜|第 \d{2} 章｜)", backlog, re.MULTILINE)
if len(chapters) != 22:
    errors.append(f"expected 22 chapters, found {len(chapters)}")
for error in errors:
    print(f"FAIL: {error}", file=sys.stderr)
print(f"docs={len(files)} local_links={links} tasks={len(tasks)} "
      f"complete={sum(state == 'x' for state, _ in tasks)} chapters={len(chapters)}")
if errors:
    sys.exit(1)
print("PASS: local targets, code fences, task IDs and chapter coverage")
