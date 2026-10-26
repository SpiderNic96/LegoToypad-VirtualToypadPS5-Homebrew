#!/usr/bin/env python3
"""Refuse a renderer ELF the ShellUI loader cannot apply (it handles RELATIVE / 64 / GLOB_DAT only)."""
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

OK = {0: "NONE", 8: "RELATIVE", 6: "GLOB_DAT", 1: "64"}   # what stable_elfldr_bridge.c applies
with open(sys.argv[1], "rb") as f:
    e = ELFFile(f)
    bad = {}
    for s in e.iter_sections():
        if isinstance(s, RelocationSection):
            for r in s.iter_relocations():
                t = r["r_info_type"]
                if t not in OK:
                    bad[t] = bad.get(t, 0) + 1
    if bad:
        sys.exit(f"renderer has relocation types the ShellUI loader cannot apply: {bad}")
    imports = set()
    dyn = e.get_section_by_name(".dynsym")
    for sym in dyn.iter_symbols():
        if sym["st_shndx"] == "SHN_UNDEF" and sym.name:
            imports.add(sym.name)
    print(f"renderer ok: {len(imports)} imports, relocations loader-safe")
