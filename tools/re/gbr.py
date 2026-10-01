#!/usr/bin/env python3
"""Read the player's own `gbr.exe`: disassemble by virtual address, find
strings, find cross-references.

    python3 tools/re/gbr.py d 0x0051c3d0 -n 40      # disassemble
    python3 tools/re/gbr.py x 0x00420b90            # who calls this
    python3 tools/re/gbr.py str "Flying::IsInAir"   # find a string and its xrefs
    python3 tools/re/gbr.py c 0x7ac068,0x7b87b4     # the strings at these addresses

**This ships no game data and never will.** It is a reader, like `imsprite` and
`imterrain` are readers: it opens the executable belonging to the person who
bought the game, on their machine, and prints what they ask about. Its *output*
must not be committed -- `docs/legal.md` rule 2 forbids pasting disassembler
output into this repository, including as comments. Describe behaviour in prose
and implement from the description.

**It needs `capstone`, which is the one third-party dependency anywhere in this
project** and is deliberately confined to this directory: nothing that builds,
tests or runs the engine imports it. `pip install capstone` when you need it.

The image base and section table are read from the PE header rather than
assumed, so a differently-linked build of the executable still resolves.
"""
import os, struct, sys, argparse

try:
    from capstone import *
except ImportError:  # pragma: no cover - a contributor tool, not a build step
    sys.exit("tools/re/gbr.py needs capstone: pip install capstone")


def default_exe():
    """The player's installation, found the way the corpus tools find it."""
    for candidate in (os.environ.get("IMPERIVM_GBR"),
                      os.path.join(os.environ.get("IMPERIVM_GAME_DIR", ""), "gbr.exe"),
                      os.path.expanduser("~/Downloads/Imperivm/gbr.exe")):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


EXE = default_exe()

class PE:
    def __init__(self, path=None):
        path = path or EXE
        if not path:
            raise SystemExit(
                "no gbr.exe found. Set IMPERIVM_GBR, or IMPERIVM_GAME_DIR to the "
                "directory holding it.")
        self.data = open(path,'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        assert d[pe:pe+4] == b'PE\0\0'
        nsec = struct.unpack_from('<H', d, pe+6)[0]
        opt = struct.unpack_from('<H', d, pe+20)[0]
        self.image_base = struct.unpack_from('<I', d, pe+24+28)[0]
        so = pe+24+opt
        self.sections=[]
        for i in range(nsec):
            o = so+i*40
            name = d[o:o+8].rstrip(b'\0').decode('latin1')
            vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', d, o+8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))
    def off(self, va):
        rva = va - self.image_base
        for name,vaddr,vsize,raddr,rsize in self.sections:
            if vaddr <= rva < vaddr+max(vsize,rsize):
                o = rva-vaddr+raddr
                if o < len(self.data): return o
        return None
    def read(self, va, n):
        o=self.off(va)
        return None if o is None else self.data[o:o+n]
    def va_of_off(self, off):
        for name,vaddr,vsize,raddr,rsize in self.sections:
            if raddr <= off < raddr+rsize:
                return self.image_base + vaddr + (off-raddr)
        return None

def disasm(pe, va, n=60, stop_at_ret=True):
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail=False
    buf = pe.read(va, max(n*8, 512))
    out=[]
    count=0
    for ins in md.disasm(buf, va):
        out.append("0x%08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
        count+=1
        if count>=n: break
        if stop_at_ret and ins.mnemonic in ('ret','retn') : break
    return "\n".join(out)

def find_bytes(pe, pat):
    res=[]
    start=0
    while True:
        i = pe.data.find(pat, start)
        if i<0: break
        res.append((i, pe.va_of_off(i)))
        start=i+1
    return res

def xrefs_to(pe, va):
    """immediate 32-bit references to va (push/mov imm) and rel32 calls."""
    target = struct.pack('<I', va)
    hits=[]
    for off,v in find_bytes(pe, target):
        if v: hits.append(('imm', v))
    # rel32 call/jmp
    md = None
    for name,vaddr,vsize,raddr,rsize in pe.sections:
        if name != '.text': continue
        base = pe.image_base+vaddr
        d = pe.data[raddr:raddr+rsize]
        for i in range(len(d)-5):
            if d[i] in (0xE8,0xE9):
                rel = struct.unpack_from('<i', d, i+1)[0]
                if base+i+5+rel == va:
                    hits.append(('call' if d[i]==0xE8 else 'jmp', base+i))
    return hits

if __name__ == '__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['d','s','x','str','c'])
    ap.add_argument('arg')
    ap.add_argument('-n', type=int, default=60)
    ap.add_argument('--nostop', action='store_true')
    a=ap.parse_args()
    pe=PE()
    if a.cmd=='d':
        print(disasm(pe, int(a.arg,0), a.n, not a.nostop))
    elif a.cmd=='s':
        pat=a.arg.encode('latin1')
        for off,va in find_bytes(pe, pat)[:50]:
            print("off=0x%06x va=0x%08x" % (off, va or 0))
    elif a.cmd=='str':
        pat=a.arg.encode('latin1')+b'\0'
        for off,va in find_bytes(pe, pat)[:50]:
            print("string at va=0x%08x" % (va or 0))
            if va:
                for k,v in xrefs_to(pe, va)[:30]:
                    print("   %s 0x%08x" % (k,v))
    elif a.cmd=='c':
        # Read the NUL-terminated string at a virtual address. The registration
        # sites push pointers, so this is what turns a `push 0x7ac068` into the
        # signature it names -- the step that was being done by hand.
        for text in a.arg.split(','):
            va = int(text, 0)
            raw = pe.read(va, 512) or b''
            print("0x%08x  %r" % (va, raw.split(b'\0')[0].decode('latin1')))
    elif a.cmd=='x':
        for k,v in xrefs_to(pe, int(a.arg,0))[:60]:
            print("%s 0x%08x" % (k,v))
