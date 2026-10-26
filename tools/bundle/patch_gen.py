#!/usr/bin/env python3
# SPDX-License-Identifier: MIT  (the output extends LegoToypad's MIT generate_assets.py, (C) 2026 Sofiane Belkacem Nacer)
"""Make ltp_generate.py: LegoToypad's own generate_assets.py (MIT, harrysof) plus a few lines
that also dump the data model it builds (franchises, rosters, vehicle builds, assets) as
ps5_model.json, which tools/build_bundle.py turns into the phone/console bundle.

usage: patch_gen.py <LegoToypad>/generate_assets.py <out>/ltp_generate.py"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()
old = """    return 0


def main():"""
assert old in s, "generate_assets.py changed: update tools/bundle/patch_gen.py"
dump = '''    # --- PS5 bundle export: dump the data model the C++ tables are built from ---
    import json as _json
    def _sym(x):
        return None if x is None else {"id": x["id"], "name": x["name"], "path": str(x["path"]) if x["path"] else None}
    def _entry(e):
        return {"name": e["name"], "build": e["build"], "bin": _sym(e["bin"]), "png": _sym(e["png"]),
                "abilityIndices": e.get("abilityIndices", [])}
    _dump = {
        "franchises": [{"name": f["name"], "logo": _sym(f["logosym"]),
                        "characters": [_entry(c) for c in f["characters"]],
                        "vehicles": [{"base": g["base"], "builds": [_entry(b) for b in g["builds"]]} for g in f["vehicles"]]}
                       for f in franchises],
        "abilities": [{"name": a["name"], "section": a["section"], "icon": _sym(a["icon"]), "ringColor": a["ringColor"]}
                      for a in ability_table],
        "symbols": {n: _sym(v) for n, v in symbols.by_name.items()},
        "version": APP_VERSION,
    }
    (out_dir / "ps5_model.json").write_text(_json.dumps(_dump))
    return 0


def main():'''
open(dst, "w", encoding="utf-8").write(s.replace(old, dump, 1))
