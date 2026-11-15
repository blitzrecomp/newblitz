#!/usr/bin/env python3
"""Bake the site in place: shared head/header/footer, config links, the trailer, screenshots, sitemap.xml.

    python3 site/tools/build.py          # from anywhere; standard library only (Pillow, if present, adds web copies of shots)

Idempotent: every generated part sits between markers (<!--@head--> ... <!--/@head-->, and so on) or is an element with a
data- attribute, so running it again only refreshes them. Settings: site/site.json. Screenshots: site/assets/shots/shots.json
(see site/README.md).
"""
import hashlib
import html
import json
import re
import sys
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
CFG = json.loads((SITE / "site.json").read_text())
SHOTS_DIR = SITE / "assets/shots"

PAGES = ["index", "download", "start", "features", "season", "contribute", "faq", "press", "privacy", "legal", "404"]
NAV = [("index", "Home", "/"), ("download", "Download", "/download.html"), ("start", "Get started", "/start.html"),
       ("features", "Features", "/features.html"), ("season", "2026 Season", "/season.html"), ("faq", "FAQ", "/faq.html"),
       ("contribute", "Contribute", "/contribute.html")]

esc = lambda s: html.escape(str(s), quote=True)


def asset_version():
    h = hashlib.sha256()
    for p in sorted((SITE / "assets/css").glob("*.css")) + sorted((SITE / "assets/js").glob("*.js")):
        h.update(p.read_bytes())
    return h.hexdigest()[:10]


def link(key):
    """data-link values: "repo", "repo:/releases/latest", "discord", "youtube", "x", "trailer"."""
    name, _, path = key.partition(":")
    if name == "trailer":
        vid = CFG.get("trailerYouTubeId", "").strip()
        return f"https://www.youtube.com/watch?v={vid}" if vid else CFG["youtube"]
    return CFG[name].rstrip("/") + path


# ---- screenshots -------------------------------------------------------------------------------------------------

def load_shots():
    """shots.json, normalised to [{slots, file, caption, alt, src, full, w, h}]. Accepts a list of objects, {"shots": [...]},
    or {slot: {...}}; an entry names its slot(s) with "slot" (a string or a list), "slots", "name" or "id", or else
    by its file name without the extension. "caption" is shown; "what" (or "alt"/"shows") is the alt text."""
    f = SHOTS_DIR / "shots.json"
    if not f.is_file():
        return None
    data = json.loads(f.read_text())
    if isinstance(data, dict):
        data = data.get("shots", None) if "shots" in data else [
            dict(v, slot=k) if isinstance(v, dict) else {"slot": k, "file": v} for k, v in data.items()]
    out = []
    for e in data or []:
        if not isinstance(e, dict) or not e.get("file"):
            continue
        slots = e.get("slots") or e.get("slot") or e.get("name") or e.get("id") or Path(e["file"]).stem
        slots = [slots] if isinstance(slots, str) else list(slots)
        file = e["file"]
        full = file if file.startswith("/") else "/assets/shots/" + file.lstrip("./")
        caption = e.get("caption") or ""
        alt = e.get("what") or e.get("alt") or e.get("shows") or caption
        out.append({"slots": slots, "file": file, "caption": caption, "alt": alt, "full": full, "src": full,
                    "w": e.get("width"), "h": e.get("height")})
    web_copies(out)
    return out


def web_copies(shots):
    """With Pillow: a 1280-wide WebP beside each shot (assets/shots/web/), used on the pages; the original stays the link."""
    try:
        from PIL import Image
    except ImportError:
        return
    for s in shots:
        src = SITE / s["full"].lstrip("/")
        if not src.is_file():
            print(f"warning: shots.json names {s['file']}, which isn't in assets/shots", file=sys.stderr)
            continue
        dst = SHOTS_DIR / "web" / (src.stem + ".webp")
        if not dst.is_file() or dst.stat().st_mtime < src.stat().st_mtime:
            dst.parent.mkdir(exist_ok=True)
            im = Image.open(src)
            im = im.convert("RGB")
            if im.width > 1280:
                im = im.resize((1280, round(im.height * 1280 / im.width)), Image.LANCZOS)
            im.save(dst, "WEBP", quality=82, method=6)
        with Image.open(dst) as im:
            s["w"], s["h"] = im.size
        s["src"] = "/" + str(dst.relative_to(SITE))


def shot_inner(slot, label, shot):
    if not shot:
        return (f'<div class="shot-frame is-empty" role="img" aria-label="Screenshot coming: {esc(label)}">'
                f'<img src="/assets/img/shot_placeholder.webp" width="960" height="540" alt="" loading="lazy" decoding="async">'
                f'<span class="shot-slot"><span class="shot-tag">Screenshot</span><span class="shot-label">{esc(label)}</span></span>'
                f'</div><figcaption>{esc(label)}</figcaption>')
    size = f' width="{shot["w"]}" height="{shot["h"]}"' if shot.get("w") and shot.get("h") else ""
    return (f'<a class="shot-frame" href="{esc(shot["full"])}"><img src="{esc(shot["src"])}"{size} alt="{esc(shot["alt"])}" '
            f'loading="lazy" decoding="async"></a><figcaption>{esc(shot["caption"] or label)}</figcaption>')


FIGURE = re.compile(r'<figure\b([^>]*\bdata-shot="([^"]+)"[^>]*)>.*?</figure>', re.S)
ATTR = lambda name: re.compile(r'\b' + name + r'="([^"]*)"')


def bake_shots(page, shots):
    by_slot = {}
    for s in shots or []:
        for slot in s["slots"]:
            by_slot.setdefault(slot, s)

    def one(m):
        attrs, slot = m.group(1), m.group(2)
        label = html.unescape(ATTR("data-label").search(attrs).group(1))
        shot = by_slot.get(slot)
        cls = ATTR("class").search(attrs).group(1)
        cls = " ".join(c for c in cls.split() if c != "has-shot") + (" has-shot" if shot else "")
        attrs = ATTR("class").sub(f'class="{cls}"', attrs, count=1)
        return f"<figure{attrs}>{shot_inner(slot, label, shot)}</figure>"

    return FIGURE.sub(one, page)


def gallery(shots):
    if not shots:
        return ('<p class="gallery-empty">The screenshot gallery fills in at launch. Until then, the '
                '<a href="/features.html">features page</a> shows where each one goes.</p>')
    items = "".join(f'<figure class="shot has-shot">{shot_inner(s["slots"][0], s["caption"], s)}</figure>' for s in shots)
    return f'<div class="gallery">{items}</div>'


# ---- shared parts ------------------------------------------------------------------------------------------------

def head(name, meta, ver, shots_state):
    site = CFG["siteUrl"].rstrip("/")
    url = site + ("/" if name == "index" else f"/{name}")
    title = meta.get("title", "New Blitz 2027")
    full = title if name == "index" else f"{title} · New Blitz 2027"
    desc = meta.get("description", "")
    img = site + "/assets/img/og_image.jpg"
    robots = '\n<meta name="robots" content="noindex">' if name == "404" else f'\n<link rel="canonical" href="{esc(url)}">' 
    return f"""<!--@head-->
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(full)}</title>
<meta name="description" content="{esc(desc)}">
{robots.strip()}
<meta name="theme-color" content="#020712">
<meta name="color-scheme" content="dark">
<link rel="icon" href="/favicon.ico" sizes="any">
<link rel="icon" type="image/png" sizes="32x32" href="/favicon-32.png">
<link rel="icon" type="image/png" sizes="192x192" href="/favicon-192.png">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<meta property="og:type" content="website">
<meta property="og:site_name" content="New Blitz 2027">
<meta property="og:title" content="{esc(full)}">
<meta property="og:description" content="{esc(desc)}">
<meta property="og:url" content="{esc(url)}">
<meta property="og:image" content="{esc(img)}">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta property="og:image:alt" content="New Blitz 2027. Blitz, native, on almost any device. Free, bring your own cartridge.">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:site" content="@blitzrecomp">
<meta name="twitter:title" content="{esc(full)}">
<meta name="twitter:description" content="{esc(desc)}">
<meta name="twitter:image" content="{esc(img)}">
<meta name="nb-shots" content="{shots_state}">
<link rel="preload" href="/assets/fonts/barlow-condensed-800i.woff2" as="font" type="font/woff2" crossorigin>
<link rel="preload" href="/assets/fonts/inter-var.woff2" as="font" type="font/woff2" crossorigin>
<link rel="stylesheet" href="/assets/css/site.css?v={ver}">
<script src="/assets/js/site.js?v={ver}" defer></script>
<!--/@head-->"""


def header(name):
    items = []
    for key, label, href in NAV:
        cur = ' aria-current="page"' if key == name else ""
        items.append(f'<li><a href="{href}"{cur}>{label}</a></li>')
    return f"""<!--@header-->
<a class="skip" href="#main">Skip to content</a>
<header class="site-header">
  <div class="wrap header-row">
    <a class="brand" href="/"><img src="/assets/img/emoji/small_mark.webp" width="40" height="40" alt=""><span class="brand-name">New Blitz <span class="brand-year">2027</span></span></a>
    <nav class="site-nav" aria-label="Main"><ul>{''.join(items)}</ul></nav>
    <a class="btn btn-hot btn-sm header-cta" href="/download.html">Download</a>
  </div>
</header>
<!--/@header-->"""


def footer():
    repo = esc(CFG["repo"])
    return f"""<!--@footer-->
<footer class="site-footer">
  <div class="rule-flame" aria-hidden="true"></div>
  <div class="wrap footer-grid">
    <div class="footer-brand">
      <img src="/assets/img/logo-900.webp" width="900" height="301" alt="New Blitz 2027" loading="lazy" decoding="async">
      <p>A Blitz Recomp project. Free, non-commercial and open source (GPL-3.0).</p>
    </div>
    <nav class="footer-nav" aria-label="Footer">
      <div><h2>Play</h2><ul><li><a href="/download.html">Download</a></li><li><a href="/start.html">Get started</a></li><li><a href="/features.html">Features</a></li><li><a href="/season.html">2026 Season</a></li></ul></div>
      <div><h2>Help</h2><ul><li><a href="/faq.html">FAQ</a></li><li><a href="/start.html#controls">Controls</a></li><li><a data-link="discord" href="{esc(CFG['discord'])}">Discord</a></li><li><a data-link="repo:/issues" href="{repo}/issues">Report a bug</a></li><li><a href="/contribute.html">Contribute</a></li></ul></div>
      <div><h2>About</h2><ul><li><a href="/press.html">Press kit</a></li><li><a href="/privacy.html">Privacy</a></li><li><a href="/legal.html">Credits and legal</a></li><li><a data-link="repo" href="{repo}">Source code</a></li></ul></div>
      <div><h2>Follow</h2><ul><li><a data-link="youtube" href="{esc(CFG['youtube'])}">YouTube</a></li><li><a data-link="x" href="{esc(CFG['x'])}">X</a></li><li><a data-link="repo" href="{repo}">GitHub</a></li></ul></div>
    </nav>
  </div>
  <div class="wrap footer-notice">
    <p>NFL Blitz 2000 is a 1999 Nintendo 64 game by Midway. New Blitz 2027 is a free fan project. It is not affiliated with, endorsed or sponsored by the NFL, the NFL Players Association, any NFL club, Midway Games or its successors, Warner Bros., Nintendo or Sony. All names, logos and trademarks belong to their owners. No game assets are included: you supply your own cartridge dump.</p>
  </div>
</footer>
<!--/@footer-->"""


def trailer():
    vid = CFG.get("trailerYouTubeId", "").strip()
    if not vid:
        return f"""<!--@trailer-->
<div class="trailer is-soon">
  <img src="/assets/img/thumb_trailer.webp" width="1280" height="720" alt="" loading="lazy" decoding="async">
  <p class="trailer-soon"><span class="eyebrow">The trailer</span><span class="trailer-soon-title">Coming soon</span><a data-link="youtube" href="{esc(CFG['youtube'])}">Watch for it on YouTube</a></p>
</div>
<!--/@trailer-->"""
    vid = esc(vid)
    return f"""<!--@trailer-->
<div class="trailer" data-yt="{vid}">
  <a class="trailer-play" href="https://www.youtube.com/watch?v={vid}">
    <img src="/assets/img/thumb_trailer.webp" width="1280" height="720" alt="" loading="lazy" decoding="async">
    <span class="play-button" aria-hidden="true"></span>
    <span class="visually-hidden">Play the New Blitz 2027 trailer</span>
  </a>
</div>
<p class="trailer-note">The video plays from YouTube (youtube-nocookie.com), and loads only when you press play.</p>
<!--/@trailer-->"""


def region(page, name, content):
    pat = re.compile(r"<!--@" + name + r"-->.*?<!--/@" + name + r"-->", re.S)
    if not pat.search(page):
        return page
    return pat.sub(lambda m: content, page)


def bake_links(page):
    def one(m):
        tag = m.group(0)
        url = link(m.group(1))
        return re.sub(r'\bhref="[^"]*"', f'href="{esc(url)}"', tag, count=1) if "href=" in tag else tag[:-1] + f' href="{esc(url)}">'
    return re.sub(r'<a\b[^>]*\bdata-link="([^"]+)"[^>]*>', one, page)


def bake_text(page):
    """<span data-text="repo">…</span>: the link's address as text (inside the contributor prompts)."""
    return re.sub(r'(<span\b[^>]*\bdata-text="([^"]+)"[^>]*>)[^<]*(</span>)', lambda m: m.group(1) + esc(link(m.group(2))) + m.group(3), page)


def page_meta(page):
    m = re.search(r"<!--page(.*?)-->", page, re.S)
    return dict(re.findall(r'(\w+)="([^"]*)"', m.group(1))) if m else {}


def main():
    ver = asset_version()
    shots = load_shots()
    state = "baked" if shots is not None else "pending"
    for name in PAGES:
        f = SITE / f"{name}.html"
        if not f.is_file():
            print(f"missing {f.name}", file=sys.stderr)
            continue
        page = f.read_text()
        meta = page_meta(page)
        page = region(page, "head", head(name, meta, ver, state))
        page = region(page, "header", header(name))
        page = region(page, "footer", footer())
        page = region(page, "trailer", trailer())
        page = region(page, "gallery", "<!--@gallery-->" + gallery(shots) + "<!--/@gallery-->")
        page = bake_shots(page, shots)
        page = bake_links(page)
        page = bake_text(page)
        f.write_text(page)
    site = CFG["siteUrl"].rstrip("/")
    urls = "".join(f"  <url><loc>{site}{'/' if n == 'index' else '/' + n}</loc></url>\n" for n in PAGES if n != "404")
    (SITE / "sitemap.xml").write_text(f'<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n{urls}</urlset>\n')
    (SITE / "robots.txt").write_text(f"User-agent: *\nAllow: /\n\nSitemap: {site}/sitemap.xml\n")
    filled = sorted({slot for s in shots or [] for slot in s["slots"]})
    print(f"built {len(PAGES)} pages (assets v{ver}); shots: {state}{', ' + str(len(shots)) + ' in shots.json' if shots else ''}")
    if shots is not None:
        wanted = slot_names()
        missing = [s for s in wanted if s not in filled]
        unknown = [s for s in filled if s not in wanted]
        if missing:
            print("slots still on placeholders: " + ", ".join(missing))
        if unknown:
            print("shots.json slots no page uses (press gallery only): " + ", ".join(unknown))


def slot_names():
    names = []
    for name in PAGES:
        f = SITE / f"{name}.html"
        if f.is_file():
            for m in FIGURE.finditer(f.read_text()):
                if m.group(2) not in names:
                    names.append(m.group(2))
    return names


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--slots":
        print("\n".join(slot_names()))
    else:
        main()
