#!/usr/bin/env python3
"""Build the LegoToypad phone remote bundle served by the PS5 Virtual Toy Pad payload.

Mirrors LegoToypad's BuildCatalogJson()/EntryJson() (main.cpp) from the same data model
its generate_assets.py turns into C++ tables, so the phone UI (Web/ shipped verbatim)
sees exactly the catalog the desktop app would serve.

Output (copy the whole folder to /data/ldmod/web on the PS5):
  index.html app.js style.css InterVariable.woff2   LegoToypad's phone UI, unchanged
  catalog.json                                      /api/catalog
  img/<id>                                          /img/<id> (portraits, logos, UI art)
  bins/<id>.bin                                     tag images by LegoToypad bin id
  entries.tsv   bin  kind  franchise  base  entry-json      (favorites, load lookups)
  figures.tsv   figure-id  bin  name  #color  portrait-url  (pad state for any placed figure)
  ui.tsv        key  value                                  (background url etc.)
"""
import json, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ltp_generate as g  # noqa: E402  (LegoToypad's generate_assets.py + model export)

KIND_C, KIND_V = "c", "v"
FINGERPRINT = "ps5"   # ?v= token; any stable value works, the PS5 serves only this bundle

YEAR1 = {"DC Comics", "The LEGO Movie", "The Lord of the Rings", "Back to the Future", "Portal 2",
         "The Simpsons", "Jurassic World", "Scooby-Doo!", "Legends of Chima", "The Wizard of Oz",
         "Doctor Who", "Ninjago", "Ghostbusters", "Midway Arcade"}
YEAR2 = {"DC Comics", "Ghostbusters 2016", "Adventure Time", "Mission Impossible", "Harry Potter",
         "The A-Team", "Fantastic Beasts and Where to Find Them", "Sonic the Hedgehog", "Gremlins",
         "E.T. the Extra-Terrestrial", "The LEGO Batman Movie", "Knight Rider", "The Goonies",
         "LEGO City Undercover", "The Powerpuff Girls", "Teen Titans Go!", "Beetlejuice"}
# kWebSorts order and values from main.cpp (enum Default, User, Story, Favorites, Year1, Year2, Abilities)
SORTS = [(0, "Default", "ASSET_SORT_DEFAULT", (240, 244, 250)), (4, "Year 1", "ASSET_SORT_YEAR1", (126, 217, 87)),
         (5, "Year 2", "ASSET_SORT_YEAR2", (255, 158, 74)), (6, "Abilities", "ASSET_SORT_ABILITIES", (168, 120, 255)),
         (1, "User", "ASSET_SORT_USER", (255, 64, 64)), (3, "Favorites", "ASSET_CUSTOM_BIN_ICON", (247, 184, 76)),
         (2, "Story", "ASSET_SORT_STARTER", (66, 157, 255))]
ABILITY_SECTIONS = ["Common", "Uncommon", "Exclusive", "Vehicular", "One-Timed"]
PADS = ["ASSET_PAD_BG_DEFAULT_LEFT_UPPER", "ASSET_PAD_BG_DEFAULT_CENTER", "ASSET_PAD_BG_DEFAULT_RIGHT_UPPER",
        "ASSET_PAD_BG_DEFAULT_LEFT_LOWER_LEFT", "ASSET_PAD_BG_DEFAULT_LEFT_LOWER_RIGHT",
        "ASSET_PAD_BG_DEFAULT_RIGHT_LOWER_LEFT", "ASSET_PAD_BG_DEFAULT_RIGHT_LOWER_RIGHT"]


def rgb_hex(ring):  # "RGB(0xRR, 0xGG, 0xBB)" -> "#RRGGBB"
    parts = ring[ring.index("(") + 1:ring.index(")")].split(",")
    return "#" + "".join("%02X" % int(p.strip(), 16) for p in parts)


def main(ltp_root: Path, out: Path, figid_tool: Path):
    model_dir = out.parent / (out.name + "-model")
    if g.generate(ltp_root, model_dir) != 0:
        sys.exit("generate_assets failed")
    m = json.loads((model_dir / "ps5_model.json").read_text())
    syms = m["symbols"]
    used = {}  # img id -> source path

    def url(sym_or_name):
        s = syms.get(sym_or_name) if isinstance(sym_or_name, str) else sym_or_name
        if not s:
            return "/img/0?v=" + FINGERPRINT
        if s["path"]:
            used[s["id"]] = s["path"]
        return "/img/%d?v=%s" % (s["id"], FINGERPRINT)

    bins = {}     # bin id -> tag path
    entries = []  # (bin, kind, franchise, base, json)

    def entry_json(e, franchise, kind, base):
        d = {"name": e["name"], "franchise": franchise, "build": e["build"],
             "color": rgb_hex(g.ring_color_for(e["bin"]["name"])), "portrait": url(e["png"]),
             "abilities": e["abilityIndices"], "bin": e["bin"]["id"]}
        bins[e["bin"]["id"]] = e["bin"]["path"]
        entries.append((e["bin"]["id"], kind, franchise, base, d))
        return d

    franchises = []
    lookup = {}
    for f in m["franchises"]:
        chars = [entry_json(c, f["name"], KIND_C, c["name"]) for c in f["characters"]]
        vehs = [{"base": v["base"], "franchise": f["name"],
                 "builds": [entry_json(b, f["name"], KIND_V, v["base"]) for b in v["builds"]]} for v in f["vehicles"]]
        franchises.append({"name": f["name"], "logo": url(f["logo"]), "year1": f["name"] in YEAR1,
                           "year2": f["name"] in YEAR2, "characters": chars, "vehicles": vehs})
        lookup[f["name"]] = (chars, vehs)

    # Story: the starter trio + Batmobile (BuildStoryJson)
    story_chars = [c for fr, nm in (("DC Comics", "Batman"), ("The Lord of the Rings", "Gandalf"),
                                    ("The LEGO Movie", "Wyldstyle"))
                   for c in lookup.get(fr, ([], []))[0] if c["name"] == nm]
    story_vehs = [v for v in lookup.get("DC Comics", ([], []))[1] if v["base"] == "Batmobile"][:1]

    # Special\ custom tags (BuildCustomTagList): negative ids, portraits served as /img/-n
    custom = []
    special = ltp_root / "Assets" / "Special"
    for i, folder in enumerate(sorted([d for d in special.iterdir() if d.is_dir()] if special.is_dir() else [],
                                      key=lambda d: d.name.casefold())):
        tag = next((p for p in folder.iterdir() if p.suffix.lower() == ".bin" and p.stat().st_size == 180), None)
        if not tag:
            continue
        cid = -(len(custom) + 1)
        pic = next((folder / (tag.stem + e) for e in (".png", ".jpg", ".jpeg") if (folder / (tag.stem + e)).is_file()), None)
        if pic:
            used[cid] = str(pic)
        d = {"name": tag.stem, "franchise": "Special", "build": 0, "color": rgb_hex(g.ring_color_for(tag.stem)),
             "portrait": "/img/%d?v=%s" % (cid if pic else 0, FINGERPRINT), "abilities": [], "bin": cid}
        bins[cid] = str(tag)
        entries.append((cid, KIND_C, "Special", tag.stem, d))
        custom.append(d)
    custom_tile = "ASSET_CUSTOM_TILE" if "ASSET_CUSTOM_TILE" in syms else "ASSET_CUSTOM_BIN_ICON"
    abilities_tile = "ASSET_ABILITIES_TILE" if "ASSET_ABILITIES_TILE" in syms else "ASSET_CHARACTERS_TILE"

    catalog = {
        "appName": "LEGO Dimensions Toypad", "version": m["version"] + " (PS5)", "listenerPort": 8765,
        "wordmark": url("ASSET_WORDMARK"), "byMark": url("ASSET_BY_HARRYSOF"), "background": url("ASSET_BACKGROUND"),
        "fontUrl": url("ASSET_UI_FONT_WEB" if "ASSET_UI_FONT_WEB" in syms else "ASSET_UI_FONT"),
        "yButton": url("ASSET_Y_BUTTON"), "settingsText": url("ASSET_SETTINGS_TEXT"), "worldTile": url("ASSET_WORLD_TILE"),
        "charactersTile": url("ASSET_CHARACTERS_TILE"), "loadBtn": url("ASSET_LOAD_BUTTON"),
        "clearBtn": url("ASSET_CLEAR_BUTTON"), "moveBtn": url("ASSET_MOVE_BUTTON"), "scrollBar": url("ASSET_SCROLL_BAR"),
        "favoritesIcon": url("ASSET_CUSTOM_BIN_ICON"), "customTile": url(custom_tile), "abilitiesTile": url(abilities_tile),
        "sorts": [{"id": i, "label": lb, "icon": url(ic), "color": "#%02X%02X%02X" % c} for i, lb, ic, c in SORTS],
        "abilitySections": ABILITY_SECTIONS,
        "abilities": [{"index": i, "name": a["name"], "section": a["section"], "icon": url(a["icon"]),
                       "color": rgb_hex(a["ringColor"])} for i, a in enumerate(m["abilities"]) if a["name"]],
        "story": {"name": "Starter Pack", "logo": url("ASSET_SORT_STARTER"), "characters": story_chars,
                  "vehicles": story_vehs},
        "custom": {"name": "Special", "logo": url(custom_tile), "characters": custom, "vehicles": []},
        "pads": [url(p) for p in PADS],
        "franchises": franchises,
    }

    if out.exists():
        shutil.rmtree(out)
    (out / "img").mkdir(parents=True)
    (out / "bins").mkdir()
    for name in ("index.html", "app.js", "style.css", "InterVariable.woff2"):
        shutil.copy2(ltp_root / "Web" / name, out / name)
    (out / "catalog.json").write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    url("ASSET_APPICON")
    for iid, path in used.items():
        shutil.copy2(path, out / "img" / str(iid))
    for bid, path in bins.items():
        shutil.copy2(path, out / "bins" / ("%d.bin" % bid))

    ids = {}
    order = sorted(bins)
    res = subprocess.run([str(figid_tool)] + [str(out / "bins" / ("%d.bin" % b)) for b in order],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    for b, line in zip(order, res):
        ids[b] = int(line.split("\t")[0])

    with open(out / "entries.tsv", "w", encoding="utf-8") as fh:
        for b, kind, fr, base, d in entries:
            fh.write("%d\t%s\t%s\t%s\t%s\n" % (b, kind, fr, base, json.dumps(d, ensure_ascii=False, separators=(",", ":"))))
    seen = set()
    with open(out / "figures.tsv", "w", encoding="utf-8") as fh:
        for b, kind, fr, base, d in entries:
            fid = ids.get(b, 0)
            if not fid or fid in seen:
                continue
            seen.add(fid)
            fh.write("%d\t%d\t%s\t%s\t%s\n" % (fid, b, d["name"], d["color"], d["portrait"]))
    with open(out / "ui.tsv", "w", encoding="utf-8") as fh:
        fh.write("background\t%s\nfavoritesLogo\t%s\nappIcon\t%d\n" % (
            catalog["background"], catalog["favoritesIcon"], syms["ASSET_APPICON"]["id"]))

    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print("bundle: %s  (%d images, %d tags, %d figure ids, %.1f MB)" % (
        out, len(used), len(bins), len(seen), size / 1e6))


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
