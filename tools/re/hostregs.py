#!/usr/bin/env python3
"""Every host entry point `gbr.exe` registers, with its body address and types.

    python3 tools/re/hostregs.py                 # every one of them
    python3 tools/re/hostregs.py Flying:: Squad  # filtered by substring

The executable registers its script API through three registrars, and walking
back from each call site through the pushed arguments recovers the name, the
body address and the argument type codes:

    0x00699bb0  plain      Register(registry, body, name, retType, nargs, argType...)
    0x00699eb0  suspending an extra `kind` between the name and the return type
    0x00699d20  textual    a signature string instead of type codes

The textual form prints the signature the executable itself carries, which
**names the parameters**: `void, int player, int num, ObjList list`. That is
better evidence than a list of type codes, and the receiver is counted in it --
`void, Squad sq, int nState, int nSetFlags, int nClrFlags` is the three-argument
member `sq.ClrCmd(state, set, clear)`.

Type codes, from the 35 `RegisterType` sites: 0 void, 1 int, 6 point, 7 bool,
0x0b str, 0x14 Obj, 0x15 Unit, 0x20 Query, 0x29 Squad, 0x2e Flying, 0x2a GAIKA,
0x28 SquadList. **Add 0x100 for by-reference**: 0x106 is a point by reference
and 0x128 a SquadList by reference.

This is the single most useful thing in this directory: it settles the receiver
and the arity of an entry point in one line, and the census this project keeps
in `docs/formats/vs-host-api.md` is inferred from call sites and says at the top
that the executable wins where the two disagree. It already has, repeatedly.

Ships no game data; see `tools/re/gbr.py` for the standing rule about output.
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbr import PE
pe=PE()
REGISTRARS = {0x699bb0:'plain', 0x699eb0:'suspend', 0x699d20:'text'}
def cstr(va):
    o = pe.off(va)
    if o is None: return None
    e = pe.data.find(b'\0', o)
    try: return pe.data[o:e].decode('latin1')
    except: return None
rows=[]
for name,vaddr,vsize,raddr,rsize in pe.sections:
    if name != '.text': continue
    base = pe.image_base+vaddr
    d = pe.data[raddr:raddr+rsize]
    i=0
    while i < len(d)-5:
        if d[i]==0xE8:
            rel = struct.unpack_from('<i', d, i+1)[0]
            tgt = base+i+5+rel
            if tgt in REGISTRARS:
                p=i
                items=[]
                # The registry itself reaches the registrar one of two ways,
                # and a scanner that knows only the first sees about half the
                # table. `push reg` is the cdecl form; `mov ecx, reg` is the
                # thiscall one, where the registry is never pushed at all.
                # Everything registered through `0x00699d20` uses the second,
                # so requiring the first hid every textual registration --
                # `BlockUserInput`, `ExploreCircle`, `View`, `SetShortcutSel`
                # among them.
                if p>=1 and 0x50 <= d[p-1] <= 0x57:
                    p-=1
                elif p>=2 and d[p-2]==0x8b and 0xc8 <= d[p-1] <= 0xcf:
                    p-=2
                else:
                    i+=1; continue
                # `push imm32` is tried before `push imm8`, and the order is
                # not a preference. Walking back over a push list is ambiguous
                # whenever the low byte of one instruction can start another,
                # and it can: `push 0x6a6d00` ends in the byte 0x6a, so reading
                # the narrow form first turned every body address with 0x6a in
                # it into a two-byte push and desynchronised the rest of the
                # list. `SetShortcutSel` is the one that showed it. The two
                # items that must be right -- the body and the name -- are
                # always `push imm32`, so preferring the wide form makes them
                # unambiguous.
                while len(items)<24:
                    if p>=5 and d[p-5]==0x68:
                        p-=5; items.append(struct.unpack_from('<I',d,p+1)[0])
                    elif p>=2 and d[p-2]==0x6a:
                        p-=2; items.append(d[p+1])
                    else:
                        break
                rows.append((base+i, REGISTRARS[tgt], items))
        i+=1
Q = sys.argv[1:]
# A textual registration carries three pointers -- body, name, signature -- and
# the code-typed ones carry a return type and an argument count on top of the
# first two. Demanding four items dropped the textual form entirely.
for va, kind, items in rows:
    if len(items) < (3 if kind == 'text' else 4): continue
    body, nameva = items[0], items[1]
    nm = cstr(nameva)
    if nm is None or not nm or not nm.isprintable(): continue
    if Q and not any(q.lower() in nm.lower() for q in Q): continue
    if kind == 'text':
        print("%-36s body=0x%08x %-8s %s" % (nm, body, kind, cstr(items[2])))
    else:
        print("%-36s body=0x%08x %-8s %s" % (nm, body, kind, items[2:]))
