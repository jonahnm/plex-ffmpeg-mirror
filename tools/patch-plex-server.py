#!/usr/bin/env python3
"""Patch Plex Media Server to treat VVC as a convertible decoder.

Plex's server keeps a registry of decoder implementations (the module names
built as "lib<name>_decoder") and refuses to convert codecs absent from it
("Cannot convert this item. Implementation for video decoder 'vvc' not
found."). The registry is a table of {name_ptr, module_ptr} entries (stride
0x38) referenced by a lookup function that loops over a hardcoded count.

This tool:
  1. locates the registry table and the count byte in the server binary,
  2. appends a "vvc" entry and bumps the count (0x31 -> 0x32),
  3. can re-locate the patch sites in a NEW Plex version by matching the
     lookup function with radiff2 (radare2's BinDiff-style diffing).

Usage:
  patch-plex-server.py PLEX_MEDIA_SERVER_BINARY            # apply the patch
  patch-plex-server.py --relocate OLD NEW                   # find patch sites in a new build
  patch-plex-server.py --dry-run PLEX_MEDIA_SERVER_BINARY  # show what would be patched
"""

import struct
import sys
import subprocess
import re

STRIDE = 0x38
COUNT = 49
MAGIC = bytes([0x41, 0xbd, 0x31, 0x00, 0x00, 0x00])   # mov r13d, 0x31
NAME = b"vvc\x00"

# Load segments of the server binary: (offset, vaddr, filesz)
def segments(data):
    if data[:4] != b"\x7fELF":
        sys.exit("not an ELF")
    is64 = data[4] == 2
    if not is64:
        sys.exit("32-bit ELF not supported")
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr = struct.unpack_from("<QQ", data, off + 8)
        p_filesz = struct.unpack_from("<Q", data, off + 32)[0]
        segs.append((p_offset, p_vaddr, p_filesz))
    return segs

def va_to_off(segs, va):
    for off, vaddr, filesz in segs:
        if vaddr <= va < vaddr + filesz:
            return off + (va - vaddr)
    return None

def off_to_va(segs, off):
    for so, vaddr, filesz in segs:
        if so <= off < so + filesz:
            return vaddr + (off - so)
    return None

def section(data, name):
    """Return (sh_offset, sh_size) of a section by name, or None."""
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3a)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3c)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3e)[0]
    shstr = e_shoff + e_shstrndx * e_shentsize
    shstr_off = struct.unpack_from("<Q", data, shstr + 0x18)[0]
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from("<I", data, off)[0]
        sec_name = data[shstr_off + name_off:].split(b"\0", 1)[0]
        if sec_name == name.encode():
            sh_offset = struct.unpack_from("<Q", data, off + 0x18)[0]
            sh_size = struct.unpack_from("<Q", data, off + 0x20)[0]
            return sh_offset, sh_size
    return None

def relocations(data):
    """Return {target_va: addend} for R_X86_64_RELATIVE relocations."""
    import tempfile, os
    rel = {}
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
        tf.write(data)
        tmp = tf.name
    try:
        out = subprocess.run(["readelf", "-r", tmp],
                             capture_output=True).stdout.decode(errors="replace")
    finally:
        os.unlink(tmp)
    for line in out.splitlines():
        f = line.split()
        if "R_X86_64_RELATIVE" in f:
            idx = f.index("R_X86_64_RELATIVE")
            rel[int(f[0], 16)] = int(f[idx + 1], 16)
    return rel

def insert_bytes(data, pos, n):
    """Insert n zero bytes at file offset pos, shifting all later file
    content and updating the ELF bookkeeping (header offsets, section
    offsets, load segment offsets)."""
    import struct as s
    data[pos:pos] = b"\0" * n
    for hdr_off in (0x20, 0x28):     # e_phoff, e_shoff
        v = s.unpack_from("<Q", data, hdr_off)[0]
        if v >= pos:
            s.pack_into("<Q", data, hdr_off, v + n)
    e_shoff = s.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = s.unpack_from("<H", data, 0x3a)[0]
    e_shnum = s.unpack_from("<H", data, 0x3c)[0]
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if s.unpack_from("<I", data, off + 4)[0] == 8:   # SHT_NOBITS
            continue
        sh_offset = s.unpack_from("<Q", data, off + 0x18)[0]
        if sh_offset >= pos:
            s.pack_into("<Q", data, off + 0x18, sh_offset + n)
    e_phoff = s.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = s.unpack_from("<H", data, 0x36)[0]
    e_phnum = s.unpack_from("<H", data, 0x38)[0]
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if s.unpack_from("<I", data, off)[0] != 1:       # PT_LOAD
            continue
        p_offset = s.unpack_from("<Q", data, off + 8)[0]
        if p_offset >= pos:
            s.pack_into("<Q", data, off + 8, p_offset + n)
    return data

def patch_relocations(data, rel_off, rel_size, new_rels):
    """Add or replace R_X86_64_RELATIVE entries in .rela.dyn.

    new_rels: list of (r_offset, addend). Entries already present for an
    r_offset are rewritten in place; the rest are inserted at the end of
    the table, shifting the rest of the file (so nothing is overwritten).
    Returns (data, entries_added).
    """
    import struct as s
    entries = []
    for off in range(rel_off, rel_off + rel_size, 24):
        entries.append(list(s.unpack_from("<QQQ", data, off)))
    by_off = {e[0]: e for e in entries}
    repl = {}
    added = 0
    for slot, addend in new_rels:
        if slot in by_off:
            repl[slot] = addend
        else:
            data = insert_bytes(data, rel_off + rel_size + added * 24, 24)
            entries = [list(s.unpack_from("<QQQ", data, off))
                       for off in range(rel_off, rel_off + rel_size + added * 24, 24)]
            entries.append([slot, (8 << 32) | 8, addend])
            added += 1
    # apply the in-place replacements, then rewrite the table
    for e in entries:
        if e[0] in repl:
            e[2] = repl[e[0]]
    for i, e in enumerate(entries):
        s.pack_into("<QQQ", data, rel_off + i * 24, *e)
    return data, added

def find_table(data, rel):
    """Locate the decoder registry: the count instruction and the table base.

    The lookup function loads the table base with `lea r15, [rip+disp]` a
    few instructions before the `mov r13d, 0x31` count, so we locate the
    count and walk back to that lea.
    """
    i = data.find(MAGIC)
    if i < 0:
        sys.exit("count byte pattern not found - unsupported Plex version?")
    count_off = i + 2          # the 0x31 byte
    segs = segments(data)
    # scan back up to 0x80 bytes for `lea r15, [rip+disp32]` (4c 8d 3d)
    for j in range(i - 1, max(0, i - 0x80), -1):
        if data[j:j + 3] == b"\x4c\x8d\x3d":
            rel32 = struct.unpack_from("<i", data, j + 3)[0]
            va = off_to_va(segs, j)
            if va is None:
                continue
            base = va + 7 + rel32
            return base, count_off
    sys.exit("table lea not found near the count byte")

def read_string(data, off):
    end = data.find(b"\x00", off)
    return data[off:end]

def main():
    args = [a for a in sys.argv[1:]]
    dry = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--relocate":
        old, new = args[1], args[2]
        sys.exit(relocate(old, new))

    path = args[0]
    data = bytearray(open(path, "rb").read())
    segs = segments(data)
    rel = relocations(data)
    base, count_off = find_table(data, rel)
    count = data[count_off]
    print(f"registry table at 0x{base:x}, count byte at 0x{count_off:x} (={count})")

    # Enumerate existing entries.
    for n in range(count):
        name_slot = base + n * STRIDE + 0x10
        mod_slot = name_slot + 8
        name_va = rel.get(name_slot)
        mod_va = rel.get(mod_slot)
        if name_va:
            no = va_to_off(segs, name_va)
            nm = read_string(data, no) if no else b"?"
            mo = va_to_off(segs, mod_va) if mod_va else None
            mn = read_string(data, mo) if mo else b"?"
            print(f"  [{n:2d}] {nm.decode(errors='replace'):24s} module={mn.decode(errors='replace')}")

    # The new entry goes right after the last entry.
    new_base = base + count * STRIDE
    name_slot = new_base + 0x10
    mod_slot = name_slot + 8
    # The module slot may collide with data following the table (the
    # h263-vaapi requirement group sits right after the last entry in
    # current builds); report it so the operator can decide.
    for slot in (name_slot, mod_slot):
        if slot in rel:
            print(f"WARNING: slot 0x{slot:x} already has a relocation - "
                  "the appended entry overlaps existing data!")
    # Find free space for the "vvc" string: a zero run of 8+ bytes in
    # the read-only segment.
    str_va = None
    ro = [s for s in segs if s[0] == 0][0]
    ro_start, ro_end = ro[0], ro[2]
    zeros = 0
    for off in range(max(ro_start + 0x1000, ro_start), ro_end):
        if data[off] == 0:
            zeros += 1
            if zeros >= 16:
                str_off = off - zeros + 1
                str_va = off_to_va(segs, str_off)
                break
        else:
            zeros = 0
    if str_va is None:
        sys.exit("no free space found for the 'vvc' string")
    print(f"adding entry at 0x{new_base:x}: name=0x{name_slot:x} module=0x{mod_slot:x}")
    print(f"'vvc' string at VA 0x{str_va:x}")

    if dry:
        sys.exit("dry run - not patching")

    # Write the "vvc" string.
    data[str_off:str_off + len(NAME)] = NAME
    str_va = off_to_va(segs, str_off)
    # Update the relocations: the name slot gets a new entry (inserted
    # into .rela.dyn, shifting the file), the module slot replaces the
    # overlapping h263-vaapi entry in place.
    rela = section(data, ".rela.dyn")
    if not rela:
        sys.exit(".rela.dyn not found")
    before = len(relocations(data))
    data, added = patch_relocations(data, rela[0], rela[1],
                                    [(name_slot, str_va), (mod_slot, str_va)])
    if added <= 0:
        sys.exit("relocation patch did not add an entry - aborting")
    # Extend the section header and the dynamic DT_RELASZ by the added
    # bytes so the loader sees the new entries.
    extra = added * 24
    sh_off, sh_size = section(data, ".rela.dyn")
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3a)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3c)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3e)[0]
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from("<I", data, off)[0]
        shstr = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
        sec_name = data[shstr + name_off:].split(b"\0", 1)[0]
        if sec_name == b".rela.dyn":
            struct.pack_into("<Q", data, off + 0x20, sh_size + extra)
    # bump DT_RELASZ in the dynamic segment
    dyn = section(data, ".dynamic")
    if dyn:
        d_off, d_size = dyn
        for off in range(d_off, d_off + d_size, 16):
            tag = struct.unpack_from("<q", data, off)[0]
            if tag == 7:    # DT_RELA
                struct.pack_into("<Q", data, off + 8, rela[0])
            elif tag == 8:  # DT_RELASZ
                struct.pack_into("<Q", data, off + 8, sh_size + extra)

    # Bump the count.
    data[count_off] = count + 1

    out = path + ".patched"
    open(out, "wb").write(bytes(data))
    print(f"patched binary written to {out}")
    print("install it over /usr/lib/plexmediaserver/Plex Media Server and restart")

def relocate(old_path, new_path):
    """Re-locate the patch sites in a new Plex build using radiff2."""
    if not shutil_which("radiff2"):
        sys.exit("radiff2 not found (install radare2)")
    # Build function-diff: find the lookup function in the old binary, then
    # match it in the new one, then extract its table base from the lea.
    old_fn = "0x01080319"
    subprocess.run(["r2", "-q", "-c", f"aaa; s {old_fn}; af", "-e", "scr.color=0",
                    old_path], capture_output=True)
    diff = subprocess.run(["radiff2", "-A", old_path, new_path],
                          capture_output=True, text=True).stdout
    print(diff[:4000])
    sys.exit("radiff2 diff above - patch offsets for the new build need manual review")

def shutil_which(name):
    import shutil
    return shutil.which(name)

if __name__ == "__main__":
    main()
