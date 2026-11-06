#!/usr/bin/env python3
"""Check built HTML links, fragment targets, assets and required reader features."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import sys
from urllib.parse import unquote, urlsplit


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.targets = set()
        self.links = []
        self.feed(text)

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        for key in ("id", "name"):
            if attrs.get(key):
                self.targets.add(attrs[key])
        for key in ("href", "src", "poster"):
            if attrs.get(key):
                self.links.append(attrs[key])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site", nargs="?", type=Path, default=Path("site"))
    args = parser.parse_args()
    root = args.site.resolve()
    pages = {path: Page(path.read_text(encoding="utf-8"))
             for path in root.rglob("*.html") if path.is_file()}
    errors = []
    checked = 0
    external = set()
    for path, page in pages.items():
        for href in page.links:
            parts = urlsplit(href)
            if parts.scheme or parts.netloc:
                if parts.scheme in {"http", "https"}:
                    external.add(href)
                continue
            target = ((root / unquote(parts.path).lstrip("/")) if parts.path.startswith("/")
                      else (path.parent / unquote(parts.path))).resolve() if parts.path else path
            if target.is_dir():
                target /= "index.html"
            checked += 1
            try:
                target.relative_to(root)
            except ValueError:
                errors.append(f"{path.relative_to(root)}: link escapes site: {href}")
                continue
            if not target.is_file():
                errors.append(f"{path.relative_to(root)}: missing target: {href}")
            elif parts.fragment and target in pages and unquote(parts.fragment) not in pages[target].targets:
                errors.append(f"{path.relative_to(root)}: missing anchor: {href}")
    required = ("index.html", "reading-path/index.html", "labs/index.html",
                "source/index.html", "search/search_index.json", "assets/site.css",
                "assets/vendor/manifest.json")
    for relative in required:
        if not (root / relative).is_file():
            errors.append(f"missing required reader output: {relative}")
    if len(pages) < 7:
        errors.append("fewer than seven rendered pages")
    manifest = root / "assets/vendor/manifest.json"
    if manifest.is_file():
        for name, record in json.loads(manifest.read_text(encoding="utf-8")).items():
            asset = manifest.parent / name
            if not asset.is_file() or hashlib.sha256(asset.read_bytes()).hexdigest() != record["sha256"]:
                errors.append(f"vendored resource missing or changed: {name}")
    for error in errors:
        print(f"FAIL: {error}", file=sys.stderr)
    print(f"pages={len(pages)} local_links_and_assets={checked} external_urls={len(external)} errors={len(errors)}")
    print("External URLs are inventoried, not network-verified; source versions remain in the documentation.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
