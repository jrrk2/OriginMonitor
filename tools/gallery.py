#!/usr/bin/env python3
"""Build the image gallery of the GitHub Pages site (docs/index.html, docs/gallery/) from the restacked finals.

    tools/gallery.py [--finals /mnt/fits-backup/claude/finals] [--out docs]

One picture per target: the restack (tools/restack.py) with the filter that has the most data. The pictures are the stacker's own previews (background extraction,
colour neutralisation, hyperbolic stretch, half resolution), unprocessed otherwise. Small targets get a close crop for the card; the card opens the full field.
The numbers in the captions come from each final's manifest.json and records.jsonl.
"""
import sys, argparse, collections, html, json, os, statistics
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)

# (final directory, slug, title, card crop as (cx, cy, width) in preview pixels or None for the whole field, full-field crop box or None)
TARGETS = [
    ("NGC_6888_Nebula",                     "ngc6888",  "Crescent Nebula, NGC 6888",        None, None),
    ("NGC_6992_Eastern_Veil_Nebula",        "ngc6992",  "Eastern Veil, NGC 6992",           (800, 298, 894), None),
    ("NGC2174_Monkey_Head_Clear",           "ngc2174",  "Monkey Head Nebula, NGC 2174",     None, None),
    ("M31_Clear",                           "m31",      "Andromeda Galaxy, M31, with M32 and M110", None, None),
    ("IC405_Flaming_Star_Clear",            "ic405",    "Flaming Star Nebula, IC 405",      None, None),
    ("IC_5070_Pelican_Nebula_Nebula",       "ic5070",   "Pelican Nebula, IC 5070",          None, None),
    ("NGC_7380_Wizard_Nebula_Nebula",       "ngc7380",  "Wizard Nebula, NGC 7380",          None, None),
    ("M45_Clear",                           "m45",      "Pleiades, M45",                    None, None),
    ("NGC_2244_Rosette_Nebula_Nebula",      "ngc2237",  "Rosette Nebula, NGC 2237/2244",    None, None),
    ("NGC_869_884_Double_Cluster_Clear",    "ngc869",   "Double Cluster, NGC 869 and 884",  None, None),
    ("M27_Nebula",                          "m27",      "Dumbbell Nebula, M27",             (761, 459, 600), None),
    ("M1_Clear",                            "m1",       "Crab Nebula, M1",                  (720, 363, 520), None),
    ("M57_Nebula",                          "m57",      "Ring Nebula, M57",                 (705, 696, 260), None),
    ("NGC_6946_Fireworks_Galaxy_Nebula",    "ngc6946",  "Fireworks Galaxy, NGC 6946",       (668, 431, 380), None),
    ("M33_Nebula",                          "m33",      "Triangulum Galaxy, M33",           None, None),
    ("M13_Clear",                           "m13",      "Hercules Cluster, M13",            (761, 509, 640), None),
    ("NGC_7000_North_America_Nebula_Nebula", "ngc7000",  "North America Nebula, NGC 7000",   None, None),
    ("NGC_1499_California_Nebula_Nebula",   "ngc1499",  "California Nebula, NGC 1499",      None, None),
    ("IC_1805_Heart_Nebula_Nebula",         "ic1805",   "Heart Nebula, IC 1805",            None, None),
    ("IC_1848_Soul_Nebula_Nebula",          "ic1848",   "Soul Nebula, IC 1848",             None, None),
    ("NGC_2264_Cone_Christmas_Tree_Nebula", "ngc2264",  "Cone Nebula and Christmas Tree Cluster, NGC 2264", None, None),
    ("M82_Nebula",                          "m82",      "Cigar Galaxy, M82",                (720, 400, 640), None),
    ("M97_Nebula",                          "m97",      "Owl Nebula, M97",                  (745, 430, 300), None),
]
# entries whose picture comes from elsewhere than the final's own preview (the facts still come from the final):
#   the Rosette's restack mixes a pre-filter reference night and comes out blue: the live stack (the same 375 frames) renders it properly
IMAGE_FROM = {"NGC_2244_Rosette_Nebula_Nebula": "../../stacks/NGC_2244_Rosette_Nebula"}
# the Crab at full resolution (tools/fullres_stack.py: quad-band colour, broadband + quad-band luminance), cropped to the Crab and its surroundings
FULLRES = {"M1_Clear": "fullres_M1/M1_combined"}
FILTER = {"Clear": "no filter", "Nebula": "Antlia quad-band nebula filter"}
CARD_W, CARD_H = 720, 480


def facts(d):
    m = json.load(open(os.path.join(d, "manifest.json")))
    s = m["stack"]
    recs = [json.loads(l) for l in open(os.path.join(d, "records.jsonl")) if l.strip()]
    good = [r for r in recs if r.get("verdict") in ("clear", "hazy", "patchy")]
    subs = collections.Counter((r.get("exposure"), r.get("iso")) for r in good).most_common(1)
    hfr = [r["hfr_arcsec"] for r in good if r.get("hfr_arcsec")]
    return {"frames": s["frames"], "minutes": s["total_exposure_s"] / 60, "nights": sorted(m["nights"]), "filter": s.get("ref_filter") or m.get("filter"),
            "sub": subs[0][0] if subs else None, "hfr": statistics.median(hfr) if hfr else None}


def night_label(nights):
    # a night is named by its evening date: "9 Oct", "11, 13 Oct", "16, 25 Oct"
    by_month = collections.OrderedDict()
    for n in nights:
        y, mo, dd = n.split("-")
        by_month.setdefault(mo, []).append(str(int(dd)))
    names = {"01": "Jan", "02": "Feb", "03": "Mar", "04": "Apr", "05": "May", "06": "Jun", "07": "Jul", "08": "Aug", "09": "Sep", "10": "Oct", "11": "Nov", "12": "Dec"}
    return "; ".join("%s %s" % (", ".join(d), names[mo]) for mo, d in by_month.items()) + " " + nights[-1][:4]


def card(im, crop):
    if crop:
        cx, cy, w = crop
        w = min(w, im.width, im.height * CARD_W / CARD_H)
        h = w * CARD_H / CARD_W
        x0 = min(max(cx - w / 2, 0), im.width - w); y0 = min(max(cy - h / 2, 0), im.height - h)
        return im.resize((CARD_W, CARD_H), Image.LANCZOS, box=(x0, y0, x0 + w, y0 + h))
    k = max(CARD_W / im.width, CARD_H / im.height)      # fill the card, trim the long side
    s = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    x0 = (s.width - CARD_W) // 2; y0 = (s.height - CARD_H) // 2
    return s.crop((x0, y0, x0 + CARD_W, y0 + CARD_H))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--finals", default="/mnt/fits-backup/claude/finals")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs"))
    a = ap.parse_args()
    gdir = os.path.join(a.out, "gallery"); os.makedirs(gdir, exist_ok=True)
    items = []
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gallery_export as GE, numpy as np
    for name, slug, title, crop, box in TARGETS:
        d = os.path.join(a.finals, name)
        if name in FULLRES:
            fd = os.path.join(a.finals, FULLRES[name]); m = json.load(open(os.path.join(fd, "manifest.json")))
            parts = [json.load(open(os.path.join(st, "manifest.json"))) for st in (c["stack"] for c in m["combined"])]
            nb = json.load(open(os.path.join(a.finals, "M1_Nebula", "manifest.json")))
            nights = sorted(set(nb.get("nights", {})) | {"2025-12-31", "2026-01-23", "2026-04-05", "2026-10-11", "2026-10-13"})   # the broadband sessions that joined (2 Jan and 6 Apr did not plate-solve)
            f = {"frames": sum(p.get("frames_stacked", 0) for p in parts), "minutes": sum(p.get("exposure_s", 0) for p in parts) / 60, "nights": nights,
                 "filter": "quad-band colour; broadband + quad-band luminance, full resolution", "sub": None, "hfr": None}
            rgb, cov, _ = GE.read_fullres(fd); H, W = cov.shape
            out, _ = GE.finish(rgb[H // 2 - 450:H // 2 + 450, W // 2 - 675:W // 2 + 675], np.ones((900, 1350), np.float32), 0.5, 2048)
            im = Image.fromarray((out * 255).astype(np.uint8))
            crop = (im.width // 2, im.height // 2, 560)              # the card shows the Crab itself
        else:
            f = facts(d)
            src = os.path.normpath(os.path.join(d, IMAGE_FROM[name])) if name in IMAGE_FROM else d
            if box:
                im = Image.open(os.path.join(src, "preview.png")).convert("RGB").crop(box)
            else:                                   # the largest rectangle the rotated frames all cover (no wedges at the corners)
                rgb, cov, _ = GE.read_state(os.path.join(src, "state.bin"))
                arr, rect = GE.from_preview(src, cov, 0.85, 4096)
                im = Image.fromarray((arr * 255).astype(np.uint8))
                if crop:                            # card crops are in the stacker preview's pixels: shift them by the extra trim
                    px0, py0, _, _ = GE.live_preview_rect(cov)
                    crop = (crop[0] - (rect[0] - px0), crop[1] - (rect[1] - py0), crop[2])
        im.save(os.path.join(gdir, slug + ".jpg"), quality=90, optimize=True, progressive=True)
        card(im, crop).save(os.path.join(gdir, slug + "_card.jpg"), quality=85, optimize=True, progressive=True)
        f.update(slug=slug, title=title, w=im.width, h=im.height, cropped=bool(crop) and crop[2] < 0.6 * im.width)   # a card that only trims a wide field is not a close crop
        items.append(f)
        print("%-10s %4d frames %5.0f min  %-6s nights %s" % (slug, f["frames"], f["minutes"], f["filter"], ",".join(f["nights"])))

    total_min = sum(i["minutes"] for i in items)
    all_nights = sorted({n for i in items for n in i["nights"]})
    cards = []
    for i in items:
        sub = "%g s at ISO %d" % i["sub"] if i["sub"] and i["sub"][0] and i["sub"][1] else ""
        bits = ["%d frames, %d min" % (i["frames"], round(i["minutes"])), sub, FILTER.get(i["filter"], i["filter"]),
                ("night of " if len(i["nights"]) == 1 else "nights of ") + night_label(i["nights"])]
        if i["hfr"]: bits.append('star HFR %.1f&Prime;' % i["hfr"])
        cards.append(
            '<figure><a href="gallery/{s}.jpg" title="Full field, {w} &times; {h}"><img src="gallery/{s}_card.jpg" alt="{t}" width="{cw}" height="{ch}" loading="lazy"></a>'
            '<figcaption><b>{t}</b>{c}<br>{b}</figcaption></figure>'.format(
                s=i["slug"], w=i["w"], h=i["h"], t=html.escape(i["title"]), cw=CARD_W, ch=CARD_H,
                c=' <span class="tag">close crop</span>' if i["cropped"] else "", b=" &middot; ".join(x for x in bits if x)))

    page = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OriginMonitor gallery</title>
<style>
:root{--bg:#0d0f13;--fg:#d9dde4;--dim:#8b93a1;--card:#161a21;--link:#8cc4ff;--tag:#2a3240}
body{background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;margin:0}
header,main,footer{max-width:1480px;margin:0 auto;padding:0 16px}
h1{font-weight:600;font-size:28px;margin:28px 0 4px}
p{color:var(--dim);max-width:900px}
a{color:var(--link)}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%%,420px),1fr));gap:20px;margin-bottom:32px}
figure{margin:0;background:var(--card);border-radius:6px;overflow:hidden}
figure img{display:block;width:100%%;height:auto;background:#000}
figcaption{padding:10px 12px 12px;font-size:14px;color:var(--dim)}
figcaption b{color:var(--fg);font-weight:600;font-size:15px}
.tag{background:var(--tag);color:var(--fg);border-radius:3px;padding:0 5px;font-size:12px;margin-left:4px}
footer{color:var(--dim);font-size:14px;padding-bottom:40px}
</style>
<header>
<h1>OriginMonitor gallery</h1>
<p>Deep-sky images taken unattended with a Celestron Origin (152 mm f/2.2 RASA, 1.48&Prime; per pixel) from Cambridge, UK, between %(first)s and %(last)s:
%(n)d targets, %(hours).1f hours of stacked exposure over %(nights)d nights. Each target is one stack of all its good frames from every night
(<code>tools/restack.py</code>), shown as the live stacker's own preview at half resolution (background extraction, colour neutralisation and a stretch, no further processing),
cropped to the area every rotated frame covers. The Crab is the exception: a full-resolution stack (<code>tools/fullres_stack.py</code>) of 4.1 hours over eight nights,
with broadband and quad-band luminance and the quad-band filter's colour; its <a href="gallery/crab_crossfade.webp">broadband-to-narrowband crossfade</a> shows what each records.
Click a picture for the full field (about 75&prime; &times; 50&prime;).</p>
<p>The stacker, scheduler and unattended supervisor are in the <a href="https://github.com/jrrk2/OriginMonitor">OriginMonitor repository</a>; how they drive the telescope is
drawn in <a href="state_machines.html">the state-machine charts</a>.</p>
</header>
<main>
%(cards)s
</main>
<footer>Photographs &copy; Jonathan Kimmitt. Star HFR is the median half-flux radius of the stacked frames. &ldquo;Antlia quad-band&rdquo; is a narrowband filter fitted on 15 October 2026; earlier frames are unfiltered.</footer>
</html>
""" % {"first": human(all_nights[0]), "last": human(all_nights[-1]), "n": len(items), "hours": total_min / 60, "nights": len(all_nights), "cards": "\n".join(cards)}
    with open(os.path.join(a.out, "index.html"), "w") as fh:
        fh.write(page)
    print("%d targets, %.1f h, %d nights -> %s" % (len(items), total_min / 60, len(all_nights), os.path.join(a.out, "index.html")))


def human(n):
    y, mo, d = n.split("-")
    return "%d %s %s" % (int(d), ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"][int(mo) - 1], y)


if __name__ == "__main__":
    main()
