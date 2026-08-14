#!/usr/bin/env python3
"""Patch Plex Media Server to treat VVC as a convertible decoder.

Plex's transcode decision checks the codec-implementation registry: a
table of {name, module} entries (stride 0x38) at 0x15a71d0, 385 entries
(count 0x181), holding the ffmpeg codec names. A codec absent from it
cannot be transcoded ("Cannot convert this item. Implementation for
video decoder 'vvc' not found.").

The bundled "Plex Transcoder" already contains a native VVC decoder
(V....D vvc), so the patch only needs to register the name. Plex's
build accidentally duplicates "eightsvx_exp" (entries 342/343); the
second copy is dead weight, so:

  1. write a "vvc" string into free rodata space,
  2. repoint the second "eightsvx_exp" entry's module slot (a plain
     RELATIVE relocation) at that string.

No bytes move, no segments change, nothing else is touched.

Usage:
  patch-plex-server.py BINARY              # writes BINARY.patched
  patch-plex-server.py --verify BINARY     # verify a patched binary
"""

import struct
import subprocess
import sys

NAME = b"vvc\x00"
R_RELATIVE = 8
SACRIFICE = b"eightsvx_exp"
DEC_LIST_BASE = 0x15a71d0
DEC_LIST_COUNT = 385


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
    if not va:
        return None
    for so, vaddr, filesz, _ in segs:
        if vaddr <= va < vaddr + filesz:
            fo = so + (va - vaddr)
            end = data.find(b"\0", fo)
            if 0 < end - fo <= limit:
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


def find_relocs(data, segs, target_str):
    rela = section(data, ".rela.dyn")
    dyn = section(data, ".dynamic")
    relasz = 0
    for off in range(dyn[0], dyn[0] + dyn[1], 16):
        if struct.unpack_from("<q", data, off)[0] == 8:
            relasz = struct.unpack_from("<Q", data, off + 8)[0]
    hits = []
    for i in range(relasz // 24):
        off = rela[0] + i * 24
        r_off, r_info, add = struct.unpack_from("<QQQ", data, off)
        if r_info & 0xffffffff != R_RELATIVE:
            continue
        if va_to_str(data, segs, add) == target_str:
            hits.append((off, r_off))
    return hits


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

    # 1. "vvc" string into free rodata
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
    data[str_off:str_off + len(NAME)] = NAME
    str_va = off_to_va(segs, str_off)
    allowed.add((str_off, len(NAME)))
    verify_clean(pristine, data, allowed)
    print(f"'vvc' string at VA 0x{str_va:x}: OK")

    # 2. find the duplicated eightsvx_exp entries and repoint the second
    #    entry's relocated fields (name at +0x08, module at +0x18/+0x20)
    hits = [h for h in find_relocs(data, segs, SACRIFICE)
            if DEC_LIST_BASE + 0x18 <= h[1] < DEC_LIST_BASE + DEC_LIST_COUNT * 0x38]
    if len(hits) != 4:
        sys.exit(f"expected 4 eightsvx_exp list relocations, found {len(hits)}")
    entries = {}
    for off, slot in hits:
        idx = (slot - DEC_LIST_BASE) // 0x38
        entries.setdefault(idx, []).append((off, slot))
    if len(entries) != 2:
        sys.exit("eightsvx_exp not duplicated as expected")
    victim = max(entries)
    victim_start = DEC_LIST_BASE + victim * 0x38
    fields = [h for h in find_relocs(data, segs, b"8svx_fib")
              if victim_start <= h[1] < victim_start + 0x38]
    if len(fields) != 1:
        sys.exit(f"expected 1 8svx_fib relocation in entry {victim}, found {len(fields)}")
    all_hits = entries[victim] + fields
    for off, slot in all_hits:
        struct.pack_into("<Q", data, off + 16, str_va)
        allowed.add((off + 16, 8))
    verify_clean(pristine, data, allowed)
    print(f"eightsvx_exp#2 (entry {victim}) repointed to 'vvc' ({len(all_hits)} fields): OK")

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
    for slot, add in rel.items():
        s = pps.va_to_str(data, segs, add)
        if s == b"vvc":
            print(f"slot {slot:#x} -> {add:#x} {s!r}")
            found += 1
    if found != 3:
        ok = False
    print("RELATIVE count:", len(rel))
    print("JUMP_SLOT count:", out.count("R_X86_64_JUMP_SLO"))
    print("VERIFY", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
