#!/usr/bin/env python3
"""Port build step: load the game's graphics/text/data from the ROM at run time.

The decomp fills its data arrays with values extracted from the ROM
(`#include "extracted/....inc"`). The port must not contain any of that data,
so for every such array this script

  * removes the #include (the array keeps its declared size; any literal
    values written in the source, e.g. Samus' graphics size header, stay),
  * puts the array into the writable section .data.mzm_asset, and
  * appends a registration function to the translation unit that tells the
    port where in the ROM the array's contents are (port/host_assets.c copies
    them from the user's ROM when the game starts).

Usage: pp_assets.py <in.c> <out.c> <function name> --region eu
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
ARRAY_RE = re.compile(r'^(?P<head>[^\n;{}]*?)=\s*\{(?P<body>[^{}]*?#include "extracted/[^"]+"[^{}]*?)\};', re.M | re.S)
HEAD_RE = re.compile(r'^(?P<pre>\s*(?:static\s+)?const\s+(?P<type>u8|u16|u32|s8|s16|s32)\s+(?P<name>\w+)\s*)(?P<dims>(?:\[[^\]]*\])+)\s*$', re.S)
ELEM = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4}


def load_db(region):
    with open(os.path.join(ROOT, "database.json")) as f:
        entries = json.load(f)
    db = {}
    for e in entries:
        db[os.path.join(e.get("dir", "data"), e["path"])] = e
    return db


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("func")
    ap.add_argument("--region", default="eu")
    args = ap.parse_args()

    db = load_db(args.region)
    text = open(args.src).read()
    regs = []

    def repl(m):
        head = m.group("head").rstrip()
        body = m.group("body")
        hm = HEAD_RE.match(head)
        if not hm:
            sys.exit("%s: cannot parse asset array head: %r" % (args.src, head))
        inc = re.search(r'#include "extracted/([^"]+)\.inc"', body).group(1)
        e = db.get(inc)
        if e is None:
            # Not part of the game data (only used by the GBA build, e.g. the
            # NES emulator payload inside #ifndef PORT): drop it.
            return "%s = {0};" % head
        prefix = re.sub(r'#include "extracted/[^"]+"', "", body)
        prefix = re.sub(r"//[^\n]*", "", prefix).strip()
        prefix_count = len([v for v in prefix.split(",") if v.strip()])
        addr = e["addr"].get(args.region)
        count = e["count"]
        if isinstance(count, dict):
            count = count.get(args.region, "0x0")
        nbytes = int(count, 16) * e["size"]
        dims = hm.group("dims")
        name = hm.group("name")
        if dims.replace(" ", "") == "[]":
            # Unsized: give it the extracted size explicitly.
            elem = int(nbytes / e["size"])
            dims = "[%d]" % (prefix_count + elem)
        new_head = "%s%s __attribute__((section(\".data.mzm_asset\")))" % (hm.group("pre"), dims)
        new_body = (prefix if prefix else "0")
        if addr is not None:
            regs.append((name, prefix_count * ELEM[hm.group("type")], int(addr, 16), nbytes))
        return "%s = {%s};" % (new_head, new_body)

    text = ARRAY_RE.sub(repl, text)
    if "#include \"extracted/" in text:
        sys.exit("%s: unconverted extracted include left" % args.src)

    out = [text, "", "/* Port: ROM locations of this file's data (tools/port/pp_assets.py). */",
           "void PortAsset_Add(void *dst, unsigned capacity, unsigned prefixBytes, unsigned romOffset, unsigned size);",
           "void %s(void)" % args.func, "{"]
    for name, prefix_bytes, addr, nbytes in regs:
        # Arrays inside inactive #if blocks don't exist; guard with the
        # same region macros is not possible here, so only register arrays
        # that have an address for this region (others are compiled out).
        out.append("    PortAsset_Add((void *)%s, sizeof(%s), %d, 0x%x, %d);" % (name, name, prefix_bytes, addr, nbytes))
    out.append("}")
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
