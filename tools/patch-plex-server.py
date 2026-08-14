#!/usr/bin/env python3
"""Patch Plex Media Server to treat VVC as a convertible decoder.

Plex's server keeps a decoder-implementation registry: a table of
{name, module} entries (stride 0x38) walked by a lookup function that
loops over a hardcoded count. Codecs absent from it cannot be transcoded
("Cannot convert this item. Implementation for video decoder ... not
found.").

The patch is entirely in-place (no byte ever moves):
  1. bumps the lookup count (0x31 -> 0x32),
  2. writes a "vvc" string and a "libvvc_decoder.so" string into free
     rodata space,
  3. points the new table entry's module slot at the module string by
     replacing an existing relocation's addend,
  4. appends one RELATIVE relocation (name slot -> "vvc") into the slack
     that Plex's build leaves at the end of .rela.dyn, bumping
     DT_RELASZ and RELACOUNT by one.

Usage:
  patch-plex-server.py BINARY              # writes BINARY.patched
  patch-plex-server.py --verify BINARY     # verify a patched binary
"""

import struct
import subprocess
import sys

STRIDE = 0x38
NAME = b"vvc\x00"
MOD_NAME = b"libvvc_decoder.so\x00"
R_RELATIVE = 8


def segments(data):
    if data[:4] != b"\x7fELF":
        sys.exit("not an ELF")
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, off)[0] != 1:   # PT_LOAD
            continue
        p_offset, p_vaddr = struct.unpack_from("<QQ", data, off + 8)
        p_filesz = struct.unpack_from("<Q", data, off + 32)[0]
        p_memsz = struct.unpack_from("<Q", data, off + 40)[0]
        segs.append((p_offset, p_vaddr, p_filesz, p_memsz))
    return segs


def off_to_va(segs, off):
    for so, vaddr, filesz, _ in segs:
        if so <= off < so + filesz:
            return vaddr + (off - so)
    return None


def section(data, name):
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3a)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3c)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3e)[0]
    shstr = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from("<I", data, off)[0]
        if data[shstr + name_off:].split(b"\0", 1)[0] == name.encode():
            return (struct.unpack_from("<Q", data, off + 0x18)[0],
                    struct.unpack_from("<Q", data, off + 0x20)[0])
    return None


def locate(data):
    tail = bytes([
        0x4c, 0x0f, 0x44, 0xf8,
        0xb8, 0x81, 0x01, 0x00, 0x00,
        0x41, 0xbd, 0x31, 0x00, 0x00, 0x00,
        0x4c, 0x0f, 0x44, 0xe8,
    ])
    i = 0
    found = None
    while True:
        j = data.find(b"\x4c\x8d\x3d", i)
        if j < 0:
            break
        if data[j + 7:j + 7 + len(tail)] == tail:
            found = j
            break
        i = j + 1
    if found is None:
        sys.exit("lookup function signature not found - unsupported Plex build?")
    rel32 = struct.unpack_from("<i", data, found + 3)[0]
    base = off_to_va(segments(data), found) + 7 + rel32
    count_off = found + 18
    assert data[count_off] == 0x31
    return base, count_off


def verify_clean(pristine, data, allowed):
    n = min(len(pristine), len(data))
    bad = [i for i in range(n) if pristine[i] != data[i]]
    unexpected = [o for o in bad
                  if not any(a <= o < a + l for a, l in allowed)]
    if len(data) != len(pristine):
        unexpected.append("size")
    if unexpected:
        sys.exit(f"unexpected diffs vs pristine: {unexpected[:10]}")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--verify":
        return verify(args[1])
    no_reloc = "--no-reloc" in args
    no_table = "--no-table" in args
    no_append = "--no-append" in args
    no_relacount = "--no-relacount" in args
    no_shdrmove = "--no-shdrmove" in args
    no_move = "--no-move" in args
    args = [a for a in args if not a.startswith("--")]
    path = args[0]
    pristine = bytearray(open(path, "rb").read())
    data = bytearray(pristine)
    segs = segments(data)
    allowed = set()

    base, count_off = locate(data)
    print(f"lookup function found; table base 0x{base:x}, count byte 0x{count_off:x}")

    if no_table:
        print("SKIP count bump / strings / addend (--no-table)")
    else:
        data[count_off] = 0x32
        allowed.add((count_off, 1))
        verify_clean(pristine, data, allowed)
        print("count bump: OK")

    ro = [s for s in segs if s[0] == 0][0]
    str_off = None
    zeros = 0
    for off in range(ro[0] + 0x1000, ro[2]):
        if data[off] == 0:
            zeros += 1
            if zeros >= 32:
                str_off = off - zeros + 1
                break
        else:
            zeros = 0
    if str_off is None:
        sys.exit("no free rodata space")
    mod_off = str_off + len(NAME)
    data[str_off:str_off + len(NAME)] = NAME
    data[mod_off:mod_off + len(MOD_NAME)] = MOD_NAME
    str_va = off_to_va(segs, str_off)
    mod_va = off_to_va(segs, mod_off)
    allowed.add((str_off, len(NAME) + len(MOD_NAME)))
    verify_clean(pristine, data, allowed)
    print(f"'vvc' string at VA 0x{str_va:x}, module at VA 0x{mod_va:x}: OK")

    name_slot = base + 49 * STRIDE + 0x10
    mod_slot = name_slot + 8

    rela = section(data, ".rela.dyn")
    dyn = section(data, ".dynamic")
    def dyn_val(tag):
        for off in range(dyn[0], dyn[0] + dyn[1], 16):
            if struct.unpack_from("<q", data, off)[0] == tag:
                return struct.unpack_from("<Q", data, off + 8)[0]
        return None

    if no_table:
        print("SKIP module slot addend (--no-table)")
    else:
        n_entries = dyn_val(8) // 24
        replaced = False
        for i in range(n_entries):
            off = rela[0] + i * 24
            r_off = struct.unpack_from("<Q", data, off)[0]
            if r_off == mod_slot:
                struct.pack_into("<Q", data, off + 16, mod_va)
                replaced = True
                break
        if not replaced:
            sys.exit("module slot relocation not found")
        print("module slot addend replaced: OK")

    if no_reloc:
        print("SKIP relocation append (--no-reloc)")
    elif no_move:
        print("SKIP block move (--no-move): extension only")
        last = max(segs, key=lambda s: s[0] + s[2])
        new_rela_size = 0x1aab88
        new_off = (last[0] + last[2] + 7) & ~7
        need = new_off + new_rela_size
        if len(data) < need:
            data.extend(b"\0" * (need - len(data)))
        e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
        e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
        e_phnum = struct.unpack_from("<H", data, 0x38)[0]
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            if struct.unpack_from("<I", data, off)[0] != 1:
                continue
            p_offset = struct.unpack_from("<Q", data, off + 8)[0]
            if p_offset == last[0]:
                new_filesz = need - p_offset
                struct.pack_into("<Q", data, off + 32, new_filesz)
                if struct.unpack_from("<Q", data, off + 40)[0] < new_filesz:
                    struct.pack_into("<Q", data, off + 40, new_filesz)
    else:
        n_entries = dyn_val(8) // 24
        entries = []
        for i in range(n_entries):
            entries.append(struct.unpack_from("<QQQ", data, rela[0] + i * 24))
        if not no_append:
            entries.append((name_slot, R_RELATIVE, str_va))
        new_rela_size = len(entries) * 24

        last = max(segs, key=lambda s: s[0] + s[2])
        new_off = (last[0] + last[2] + 7) & ~7
        new_va = last[1] + (new_off - last[0])

        e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
        e_shentsize = struct.unpack_from("<H", data, 0x3a)[0]
        e_shnum = struct.unpack_from("<H", data, 0x3c)[0]
        shdr_size = e_shnum * e_shentsize
        old_shstr = section(data, ".shstrtab")
        shstr_data = bytes(data[old_shstr[0]:old_shstr[0] + old_shstr[1]])

        new_shoff = new_off + new_rela_size
        new_shoff = (new_shoff + 7) & ~7
        new_shstr_off = new_shoff + shdr_size

        if len(data) < new_shstr_off + len(shstr_data):
            data.extend(b"\0" * (new_shstr_off + len(shstr_data) - len(data)))

        if no_shdrmove:
            print("SKIP shdr/shstrtab move (--no-shdrmove)")
        else:
            data[new_shoff:new_shoff + shdr_size] = data[e_shoff:e_shoff + shdr_size]
            data[new_shstr_off:new_shstr_off + len(shstr_data)] = shstr_data
            struct.pack_into("<Q", data, 0x28, new_shoff)
            for i in range(e_shnum):
                off = new_shoff + i * e_shentsize
                if struct.unpack_from("<Q", data, off + 0x18)[0] == old_shstr[0]:
                    struct.pack_into("<Q", data, off + 0x18, new_shstr_off)

        for i, (r_off, r_info, addend) in enumerate(entries):
            struct.pack_into("<QQQ", data, new_off + i * 24, r_off, r_info, addend)

        e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
        e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
        e_phnum = struct.unpack_from("<H", data, 0x38)[0]
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            if struct.unpack_from("<I", data, off)[0] != 1:
                continue
            p_offset = struct.unpack_from("<Q", data, off + 8)[0]
            if p_offset == last[0]:
                new_filesz = new_off + new_rela_size - p_offset
                struct.pack_into("<Q", data, off + 32, new_filesz)
                if struct.unpack_from("<Q", data, off + 40)[0] < new_filesz:
                    struct.pack_into("<Q", data, off + 40, new_filesz)

        for off in range(dyn[0], dyn[0] + dyn[1], 16):
            tag = struct.unpack_from("<q", data, off)[0]
            if tag == 7:
                struct.pack_into("<Q", data, off + 8, new_va)
            elif tag == 8:
                struct.pack_into("<Q", data, off + 8, new_rela_size)
            elif tag == 0x6ffffff9 and not no_relacount:
                struct.pack_into("<Q", data, off + 8,
                                 sum(1 for _, info, _ in entries if info == R_RELATIVE))

        if not no_shdrmove:
            for i in range(e_shnum):
                off = new_shoff + i * e_shentsize
                name_off = struct.unpack_from("<I", data, off)[0]
                nm = data[new_shstr_off + name_off:].split(b"\0", 1)[0]
                if nm == b".rela.dyn":
                    struct.pack_into("<Q", data, off + 0x10, new_va)
                    struct.pack_into("<Q", data, off + 0x18, new_off)
                    struct.pack_into("<Q", data, off + 0x20, new_rela_size)
                    struct.pack_into("<Q", data, off + 0x28, 0)
        print("relocations relocated: OK")

    out = path + ".patched"
    open(out, "wb").write(bytes(data))
    print(f"patched binary written to {out}")
    return 0


def verify(path):
    data = open(path, "rb").read()
    with open("/tmp/pps_verify.bin", "wb") as f:
        f.write(data)
    out = subprocess.run(["readelf", "-r", "/tmp/pps_verify.bin"],
                         capture_output=True).stdout.decode(errors="replace")
    rel = {}
    for line in out.splitlines():
        f = line.split()
        if "R_X86_64_RELATIVE" in f:
            idx = f.index("R_X86_64_RELATIVE")
            rel[int(f[0], 16)] = int(f[idx + 1], 16)
    ok = True
    want = {0x15ae0d8: NAME, 0x15ae0e0: MOD_NAME}
    for slot, expect in want.items():
        va = rel.get(slot, 0)
        got = data[va:va + len(expect)] if va else b"MISSING"
        print(f"slot {hex(slot)} -> {hex(va)} {got!r}")
        if got != expect:
            ok = False
    print("RELATIVE count:", len(rel))
    print("JUMP_SLOT count:", out.count("R_X86_64_JUMP_SLO"))
    print("VERIFY", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
