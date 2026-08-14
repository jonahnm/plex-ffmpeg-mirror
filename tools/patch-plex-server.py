#!/usr/bin/env python3
"""Patch Plex Media Server to treat VVC as a convertible decoder.

Plex's server keeps a decoder-implementation registry: a table of
{name, module} entries (stride 0x38) walked by a lookup function that
loops over a hardcoded count. Codecs absent from it cannot be transcoded
("Cannot convert this item. Implementation for video decoder ... not
found.").

The patch is entirely in-place (no byte ever moves):
  1. writes a "vvc" string and a "libvvc_decoder.so" string into free
     rodata space,
  2. repoints the registry entry for "h264_nvenc" (NVIDIA h264 encoder
     module, dead weight on GPU-less servers) at those strings by
     replacing the two existing RELATIVE relocation addends. The lookup
     for "vvc" now matches and yields libvvc_decoder.so; everything else
     is byte-identical to the pristine binary.

Usage:
  patch-plex-server.py BINARY              # writes BINARY.patched
  patch-plex-server.py --verify BINARY     # verify a patched binary
"""

import struct
import subprocess
import sys

NAME = b"vvc\x00"
MOD_NAME = b"libvvc_decoder.so\x00"
R_RELATIVE = 8
SACRIFICE = b"h264_nvenc"


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


def va_to_str(data, segs, va, limit=64):
    for so, vaddr, filesz, _ in segs:
        if vaddr <= va < vaddr + filesz:
            fo = so + (va - vaddr)
            end = data.find(b"\0", fo)
            if end < 0 or end - fo > limit:
                return None
            return bytes(data[fo:end])
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
    path = args[0]
    pristine = bytearray(open(path, "rb").read())
    data = bytearray(pristine)
    segs = segments(data)
    allowed = set()

    # 1. strings into free rodata
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

    # 2. find the sacrificed entry ("h264_nvenc") by its string addends
    rela = section(data, ".rela.dyn")
    dyn = section(data, ".dynamic")
    relasz = 0
    for off in range(dyn[0], dyn[0] + dyn[1], 16):
        if struct.unpack_from("<q", data, off)[0] == 8:
            relasz = struct.unpack_from("<Q", data, off + 8)[0]
    n_entries = relasz // 24
    hits = []
    for i in range(n_entries):
        off = rela[0] + i * 24
        r_off, r_info, add = struct.unpack_from("<QQQ", data, off)
        if r_info & 0xffffffff != R_RELATIVE:
            continue
        s = va_to_str(data, segs, add)
        if s == SACRIFICE:
            hits.append((off, r_off))
    if len(hits) != 2:
        sys.exit(f"expected 2 h264_nvenc relocations, found {len(hits)}")
    name_off, name_slot = hits[0]
    mod_off_r, mod_slot = hits[1]
    if name_slot > mod_slot:
        name_off, name_slot, mod_off_r, mod_slot = mod_off_r, mod_slot, name_off, name_slot
    print(f"h264_nvenc entry at slots {hex(name_slot)}/{hex(mod_slot)}: OK")

    # 3. repoint name -> "vvc", module -> "libvvc_decoder.so"
    struct.pack_into("<Q", data, name_off + 16, str_va)
    struct.pack_into("<Q", data, mod_off_r + 16, mod_va)
    allowed.add((name_off + 16, 8))
    allowed.add((mod_off_r + 16, 8))
    verify_clean(pristine, data, allowed)
    print("entry repointed: OK")

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
    import importlib.util
    spec = importlib.util.spec_from_file_location("pps", __file__)
    pps = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pps)
    segs = pps.segments(data)
    ok = True
    found = 0
    want = {NAME.rstrip(b"\0"), MOD_NAME.rstrip(b"\0")}
    for slot, add in rel.items():
        s = pps.va_to_str(data, segs, add)
        if s in want:
            print(f"slot {hex(slot)} -> {hex(add)} {s!r}")
            found += 1
    if found != 2:
        ok = False
    print("RELATIVE count:", len(rel))
    print("JUMP_SLOT count:", out.count("R_X86_64_JUMP_SLO"))
    print("VERIFY", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
