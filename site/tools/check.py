#!/usr/bin/env python3
"""Check the built site: internal links and anchors, assets, HTML basics, page weight.

    python3 site/tools/check.py

Internal links: every href/src that points into the site must exist, and every #fragment must be an id on its page.
HTML basics: one <h1>, unique ids, alt on every <img>, balanced non-void tags, <html lang>, a <title>.
Page weight: the page plus everything it could load (CSS and its images, JS, fonts, images, every srcset size), uncompressed.
"""
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

SITE = Path(__file__).resolve().parents[1]
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr", "use", "path",
        "rect", "circle", "g", "symbol"}


class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links, self.assets, self.ids, self.stack, self.errors = [], [], [], [], []
        self.h1 = 0
        self.lang = self.title = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "html" and a.get("lang"):
            self.lang = True
        if tag == "title":
            self.title = True
        if tag == "h1":
            self.h1 += 1
        if "id" in a:
            self.ids.append(a["id"])
        if tag == "img" and "alt" not in a:
            self.errors.append(f"img without alt: {a.get('src')}")
        if tag == "a" and a.get("href"):
            self.links.append(a["href"])
        if tag in ("img", "script", "source") and a.get("src"):
            self.assets.append(a["src"])
        if tag == "img" and a.get("srcset"):
            self.assets += [p.strip().split()[0] for p in a["srcset"].split(",")]
        if tag == "link" and a.get("rel") in ("stylesheet", "preload", "icon", "apple-touch-icon") and a.get("href"):
            self.assets.append(a["href"])
        if tag == "use" and a.get("href"):
            self.assets.append(a["href"])
        if tag not in VOID:
            self.stack.append((tag, self.getpos()))

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and self.stack and self.stack[-1][0] == tag:
            self.stack.pop()

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack:
            self.errors.append(f"stray </{tag}> at {self.getpos()}")
            return
        if self.stack[-1][0] == tag:
            self.stack.pop()
            return
        # p and li may close implicitly; report anything else
        names = [t for t, _ in self.stack]
        if tag in names:
            while self.stack and self.stack[-1][0] != tag:
                t, pos = self.stack.pop()
                if t not in ("p", "li"):
                    self.errors.append(f"<{t}> at {pos} closed by </{tag}>")
            self.stack.pop()
        else:
            self.errors.append(f"stray </{tag}> at {self.getpos()}")


def local(url, page):
    u = urlsplit(url)
    if u.scheme or u.netloc or url.startswith(("mailto:", "data:")):
        return None, None
    path = u.path
    if not path:
        return page, u.fragment
    target = (SITE / path.lstrip("/")) if path.startswith("/") else (page.parent / path)
    if target.is_dir():
        target = target / "index.html"
    return target, u.fragment


def main():
    pages = sorted(SITE.glob("*.html"))
    parsed = {}
    for p in pages:
        pg = Page()
        pg.feed(p.read_text())
        parsed[p] = pg
    problems = 0
    css = (SITE / "assets/css/site.css").read_text()
    css_urls = re.findall(r"url\((/[^)]+)\)", css)
    for u in css_urls:
        if not (SITE / u.lstrip("/")).is_file():
            print(f"site.css: missing {u}")
            problems += 1
    print(f"{'page':16} {'html':>7} {'total':>8}  (KB, uncompressed. An upper bound: every CSS image and every srcset size count)")
    for p, pg in parsed.items():
        errs = list(pg.errors)
        if pg.h1 != 1:
            errs.append(f"{pg.h1} <h1>")
        if not pg.lang:
            errs.append("no <html lang>")
        if not pg.title:
            errs.append("no <title>")
        dup = {i for i in pg.ids if pg.ids.count(i) > 1}
        if dup:
            errs.append(f"duplicate ids {sorted(dup)}")
        for t, pos in pg.stack:
            if t not in ("html", "body", "p", "li"):
                errs.append(f"unclosed <{t}> at {pos}")
        for href in pg.links:
            target, frag = local(href, p)
            if target is None:
                continue
            if not target.is_file():
                errs.append(f"broken link {href}")
            elif frag and target.suffix == ".html":
                other = parsed.get(target)
                if other and frag not in other.ids:
                    errs.append(f"missing anchor {href}")
        files = {p}
        for src in pg.assets:
            target, frag = local(src.split("?")[0], p)
            if target is None:
                continue
            if not target.is_file():
                errs.append(f"missing asset {src}")
            else:
                files.add(target)
        files.update(SITE / u.lstrip("/") for u in set(css_urls) if (SITE / u.lstrip("/")).is_file())
        total = sum(f.stat().st_size for f in files)
        print(f"{p.name:16} {p.stat().st_size / 1024:7.1f} {total / 1024:8.1f}")
        for e in errs:
            print(f"    {e}")
        problems += len(errs)
    print("ok" if not problems else f"{problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
