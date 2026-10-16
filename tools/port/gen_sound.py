#!/usr/bin/env python3
"""Turn the GBA sound data (sound/*.s) into host-portable objects.

The sound data is written in ARM assembler (MP2K-style track macros, voice
group macros, .incbin samples). It is assembled once with arm-none-eabi-as and
the resulting ELF objects are converted into:

  <out>/sound_blobs.s    data blobs (.incbin of the raw section bytes) plus every
                         global symbol defined as blob+offset (.set). Uses only
                         directives that GNU as accepts for both x86 and ARM.
  <out>/sound_relocs.c   table of 32-bit pointer fixups and PortSoundRelocate(),
                         which writes the host addresses into the blobs at start.
  <out>/*.bin            the raw section bytes.

Pointers inside the track data are stored unaligned (e.g. after a GOTO byte),
which is why they are patched at run time instead of being expressed in C.
"""
import argparse
import json
import re
import os
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

SOURCES = ["direct_sound_data", "programmable_wave_data", "voice_groups",
           "track_data", "sound_entries", "m_play_def", "tracks/track_0"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--out", required=True)
    ap.add_argument("--region", default="eu")
    ap.add_argument("--as", dest="assembler", default="arm-none-eabi-as")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    blobs = []      # (blob symbol, bin path, size)
    symbols = []    # (name, blob symbol, offset)
    relocs = []     # (blob symbol, offset, target symbol or None, addend)
    externs = set()
    local_syms = {}  # labels some files forgot to mark .global but others reference
    sample_offsets = {}

    # Sound samples come from the user's ROM at run time: reserve their space
    # and remember where they live in the ROM.
    with open(os.path.join(args.root, "database.json")) as f:
        db = {os.path.join(e.get("dir", "data"), e["path"]): e for e in json.load(f)}
    samples = []  # (marker label, rom address, size)

    for src in SOURCES:
        obj = os.path.join(args.out, src.replace("/", "_") + ".o")
        srcpath = os.path.join(args.root, "sound", src + ".s")
        text = open(srcpath).read()
        if "direct_sound_samples" in text:
            def repl(m):
                e = db[m.group(1)]
                size = int(e["count"], 16) * e["size"]
                label = "port_dsamp_%d" % len(samples)
                samples.append((label, int(e["addr"][args.region], 16), size))
                return "%s:\n    .space %d" % (label, size)
            text = re.sub(r'\.incbin "(sound/direct_sound_samples/[^"]+)"', repl, text)
            srcpath = os.path.join(args.out, src.replace("/", "_") + ".port.s")
            with open(srcpath, "w") as f:
                f.write(text)
        subprocess.check_call([args.assembler, "-mcpu=arm7tdmi",
                               "--defsym", "REGION_%s=1" % args.region.upper(),
                               "-I", args.root, srcpath, "-o", obj])
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            symtab = elf.get_section_by_name(".symtab")
            for sec_index, sec in enumerate(elf.iter_sections()):
                if sec.name not in (".rodata", ".data") or sec.data_size == 0:
                    continue
                blob = "port_sound_%s_%s" % (src.replace("/", "_"), sec.name.strip("."))
                data = bytearray(sec.data())
                # Relocations for this section
                for rs in elf.iter_sections():
                    if not isinstance(rs, RelocationSection) or rs["sh_info"] != sec_index:
                        continue
                    for r in rs.iter_relocations():
                        rtype = r["r_info_type"]
                        if rtype != 2:  # R_ARM_ABS32
                            sys.exit("%s: unsupported relocation type %d" % (src, rtype))
                        off = r["r_offset"]
                        addend = struct.unpack_from("<I", data, off)[0]
                        sym = symtab.get_symbol(r["r_info_sym"])
                        if sym["st_shndx"] == "SHN_UNDEF":
                            relocs.append((blob, off, sym.name, addend))
                            externs.add(sym.name)
                        else:
                            target_sec = elf.get_section(sym["st_shndx"])
                            target_blob = "port_sound_%s_%s" % (src.replace("/", "_"), target_sec.name.strip("."))
                            relocs.append((blob, off, target_blob, addend + sym["st_value"]))
                        struct.pack_into("<I", data, off, 0)
                binpath = os.path.join(args.out, blob + ".bin")
                with open(binpath, "wb") as b:
                    b.write(data)
                blobs.append((blob, binpath, len(data)))
                for sym in symtab.iter_symbols():
                    if sym["st_shndx"] != sec_index or not sym.name or sym["st_info"]["type"] in ("STT_SECTION", "STT_FILE"):
                        continue
                    if sym.name.startswith("port_dsamp_"):
                        sample_offsets[sym.name] = (blob, sym["st_value"])
                    elif sym["st_info"]["bind"] == "STB_GLOBAL":
                        symbols.append((sym.name, blob, sym["st_value"]))
                    else:
                        local_syms.setdefault(sym.name, (sym.name, blob, sym["st_value"]))

    defined = {s[0] for s in symbols} | {b[0] for b in blobs}
    for e in sorted(externs):
        if e not in defined and e in local_syms:
            symbols.append(local_syms[e])
            defined.add(e)
    missing = sorted(e for e in externs if e not in defined)
    if missing:
        sys.exit("unresolved sound symbols: " + ", ".join(missing[:10]))

    with open(os.path.join(args.out, "sound_blobs.s"), "w") as s:
        s.write("/* Generated by tools/port/gen_sound.py - do not edit. */\n")
        s.write("\t.data\n")
        for blob, binpath, size in blobs:
            s.write("\t.balign 4\n\t.globl %s\n%s:\n\t.incbin \"%s\"\n" % (blob, blob, os.path.abspath(binpath)))
        for name, blob, off in symbols:
            s.write("\t.globl %s\n\t.set %s, %s + %d\n" % (name, name, blob, off))

    with open(os.path.join(args.out, "sound_relocs.c"), "w") as c:
        c.write("/* Generated by tools/port/gen_sound.py - do not edit. */\n")
        c.write("#include <stdint.h>\n#include <string.h>\n\n")
        names = sorted({b[0] for b in blobs} | {r[2] for r in relocs})
        for n in names:
            c.write("extern unsigned char %s[];\n" % n)
        c.write("\nstruct PortSoundReloc { unsigned char *where; const unsigned char *target; uint32_t addend; };\n")
        c.write("static const struct PortSoundReloc sRelocs[] = {\n")
        for blob, off, target, addend in relocs:
            c.write("    { %s + %d, %s, %du },\n" % (blob, off, target, addend))
        c.write("};\n\n")
        c.write("void PortSoundRelocate(void)\n{\n    unsigned i;\n")
        c.write("    for (i = 0; i < sizeof(sRelocs) / sizeof(sRelocs[0]); i++) {\n")
        c.write("        uint32_t v = (uint32_t)(uintptr_t)(sRelocs[i].target + sRelocs[i].addend);\n")
        c.write("        memcpy(sRelocs[i].where, &v, 4);\n    }\n}\n\n")
        c.write("void PortAsset_Add(void *dst, unsigned capacity, unsigned prefixBytes, unsigned romOffset, unsigned size);\n")
        c.write("void PortAssets_Sound(void)\n{\n")
        for label, addr, size in samples:
            blob, off = sample_offsets[label]
            c.write("    PortAsset_Add(%s + %d, %d, 0, 0x%x, %d);\n" % (blob, off, size, addr, size))
        c.write("}\n")
    print("sound: %d blobs, %d symbols, %d relocations" % (len(blobs), len(symbols), len(relocs)))


if __name__ == "__main__":
    main()
