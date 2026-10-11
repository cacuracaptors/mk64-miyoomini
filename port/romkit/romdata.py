#!/usr/bin/env python3
"""romdata.py -- the game data inside the executable (release tool, runs in WSL).

  romdata.py sections MAP BIN sections.bin
      every data section of every object (from the linker map), with its bytes from
      the executable, for "romrecipe scan" (which tells which ones are ROM data)
  romdata.py strip MAP BIN out_bin romdata.bin OBJ [OBJ ...]
      the data sections of these objects: their bytes go to romdata.bin (the game
      fills them back in at start-up, see port/setup_miyoo.c) and out_bin is the
      executable with them zeroed, the one that is published

romdata.bin: b"MK64ROMD", u32 region count, then per region u32 address and
u32 size, then the bytes of every region in that order (little-endian).
"""
import re
import struct
import sys


def load_segments(data):
    """(vaddr, offset, filesz) of the PT_LOAD segments of a 32-bit little-endian ELF"""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("not a 32-bit little-endian ELF")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def file_offset(segs, addr, size):
    for vaddr, off, filesz in segs:
        if vaddr <= addr and addr + size <= vaddr + filesz:
            return off + (addr - vaddr)
    return None


def map_sections(path):
    """[(section, address, size, object)] for the .data*/.rodata* input sections"""
    out = []
    lines = open(path, errors="replace").read().split("\n")
    started = False
    pending = None
    num = r"0x[0-9a-fA-F]+"
    for line in lines:
        if line.startswith("Linker script and memory map"):
            started = True
            continue
        if not started:
            continue
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
        sec = None
        if m:
            sec, addr, size, obj = m.group(1), m.group(2), m.group(3), m.group(4)
        elif pending:
            m = re.match(r"^\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
            if m:
                sec, addr, size, obj = pending, m.group(1), m.group(2), m.group(3)
        pending = None
        if not sec:
            continue
        if not (sec.startswith(".data") or sec.startswith(".rodata")):
            continue
        addr, size = int(addr, 16), int(size, 16)
        if addr and size:
            out.append((sec, addr, size, obj.strip()))
    return out


def cmd_sections(mapf, binf, outf):
    data = open(binf, "rb").read()
    segs = load_segments(data)
    n = 0
    with open(outf, "wb") as f:
        for sec, addr, size, obj in map_sections(mapf):
            off = file_offset(segs, addr, size)
            if off is None:
                continue
            name = ("%s %s" % (obj, sec)).encode()
            f.write(struct.pack("<I", len(name)) + name + struct.pack("<I", size) + data[off:off + size])
            n += 1
    print("%d secoes de dados em %s" % (n, outf))


def cmd_strip(mapf, binf, outbin, outrom, objs):
    data = bytearray(open(binf, "rb").read())
    segs = load_segments(bytes(data))
    want = set(objs)
    regions = []
    for sec, addr, size, obj in map_sections(mapf):
        if obj in want:
            off = file_offset(segs, addr, size)
            if off is None:
                sys.exit("%s %s is not in a loaded segment" % (obj, sec))
            regions.append((addr, size, off))
    regions.sort()
    blob = bytearray(b"MK64ROMD" + struct.pack("<I", len(regions)))
    for addr, size, _ in regions:
        blob += struct.pack("<II", addr, size)
    total = 0
    for addr, size, off in regions:
        blob += data[off:off + size]
        data[off:off + size] = bytes(size)
        total += size
    open(outrom, "wb").write(blob)
    open(outbin, "wb").write(data)
    print("%d trechos, %d bytes de dados do jogo tirados do executavel" % (len(regions), total))


if __name__ == "__main__":
    a = sys.argv
    if len(a) == 5 and a[1] == "sections":
        cmd_sections(a[2], a[3], a[4])
    elif len(a) >= 7 and a[1] == "strip":
        cmd_strip(a[2], a[3], a[4], a[5], a[6:])
    else:
        sys.exit(__doc__)
