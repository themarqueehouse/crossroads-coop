#!/usr/bin/env python3
"""Emit coop-offsets.json from the compiled offsets probe.

The test harness reads co-op state straight out of emulated memory, so it needs
the real offsets of things inside SaveBlock1. Restating the struct layout in
JavaScript drifts, and drifted twice already: sizeof(struct CoopPlayer2) is 632
rather than the 636 its fields sum to, so deriving the offset as "block size
minus record size" landed four bytes early and returned plausible nonsense.

src/coop_offsets_probe.c asks the compiler instead. Its arrays are unreferenced,
so --gc-sections keeps them out of the ROM entirely and this costs nothing at
runtime.

Symbols are located by address rather than by assuming they sit in the order
they were declared -- which they do not. The compiler emitted the second array
first, and a reader that assumed otherwise got the first array's values for the
second array's names: all plausible numbers, all wrong.

    make modern
    python3 tools/coop/emit_offsets.py coop/harness/coop-offsets.json
"""

import json
import re
import struct
import subprocess
import sys
from pathlib import Path

OBJ = Path("build/emerald/src/coop_offsets_probe.o")
ELF = Path("pokeemerald.elf")

# EWRAM symbols the harness has to find. These used to be copied out of the
# linker map into stage1.mjs by hand, which is a constant that goes stale on
# every build that moves anything -- and goes stale silently, because a wrong
# EWRAM address reads as plausible zeroes rather than failing.
#
# The file-local gate statics are in here too. A test that cannot see the state
# it is checking has to infer it from behaviour, and the behaviour is what is
# under test.
WANTED_SYMBOLS = {
    # The blocks themselves, not gSaveBlock1Ptr: the pointers live in IWRAM,
    # and the harness addresses memory as one run of EWRAM from the mailbox
    # outwards, so an IWRAM address is not somewhere it can reach.
    "gNetMailbox": "mailboxAddr",
    "gSaveblock1": "saveBlock1Addr",
    "gSaveblock2": "saveBlock2Addr",
    "sGateId": "gateIdAddr",
    "sGateSeq": "gateSeqAddr",
    "sGateOpen": "gateOpenAddr",
    "sGateSendId": "gateSendIdAddr",
    "sGateSendSeq": "gateSendSeqAddr",
    "sGateSendsLeft": "gateSendsLeftAddr",
    "sPeerGateId": "peerGateIdAddr",
    "sPeerGateSeq": "peerGateSeqAddr",
    "sUsedPeerGateSeq": "usedPeerGateSeqAddr",
    "gSpecialVar_LastTalked": "lastTalkedAddr",
}

# Symbol -> the field names its entries carry, in order.
ARRAYS = {
    "gCoopOffsets": [
        "sizeofSaveBlock1", "sizeofCoopPlayer2", "coopPlayer2",
        "playerName", "playerGender", "partyCount", "claimed",
        "pos", "location", "party",
    ],
    "gCoopWorldOffsets": [
        "flags", "numFlagBytes", "dexSeen", "dexCaught", "numDexFlagBytes",
        "flagBadge01", "tempFlagsSize", "bag", "encryptionKey",
        "sb1Location", "warpMapNum",
    ],
}


def symbols():
    """Offset and size of each defined symbol in the object's .rodata."""
    out = subprocess.run(
        ["arm-none-eabi-nm", "-S", "--defined-only", str(OBJ)],
        capture_output=True, text=True,
    ).stdout
    found = {}
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+)\s+([0-9a-f]+)\s+\S\s+(\S+)$", line.strip())
        if m:
            found[m.group(3)] = (int(m.group(1), 16), int(m.group(2), 16))
    return found


def ewram_symbols():
    """Address of each WANTED_SYMBOLS entry, from the linked ELF.

    Local statics are in the ELF symbol table too -- lowercase 'b' rather than
    'B' -- so the gate state is reachable without exporting it from the module
    just to be testable.
    """
    out = subprocess.run(
        ["arm-none-eabi-nm", "--defined-only", str(ELF)],
        capture_output=True, text=True,
    ).stdout
    found = {}
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]{8})\s+\S\s+(\S+)$", line.strip())
        if m and m.group(2) in WANTED_SYMBOLS:
            found[WANTED_SYMBOLS[m.group(2)]] = int(m.group(1), 16)
    return found


def main(argv):
    out_path = Path(argv[1]) if len(argv) > 1 else Path("coop/harness/coop-offsets.json")

    if not OBJ.exists():
        print(f"error: {OBJ} not found. Run `make modern` first.", file=sys.stderr)
        return 1

    rodata = subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", "--only-section=.rodata",
         str(OBJ), "/dev/stdout"],
        capture_output=True,
    ).stdout

    syms = symbols()
    data = {}

    for name, fields in ARRAYS.items():
        if name not in syms:
            print(f"error: {name} not in {OBJ}. Did the probe change?", file=sys.stderr)
            return 1

        off, size = syms[name]
        want = len(fields) * 4
        if size != want:
            print(
                f"error: {name} is {size} bytes, expected {want} for "
                f"{len(fields)} entries. src/coop_offsets_probe.c and this "
                "script disagree about its contents.",
                file=sys.stderr,
            )
            return 1

        values = struct.unpack(f"<{len(fields)}I", rodata[off:off + size])
        data.update(zip(fields, values))

    # Checks the harness cannot make for itself.
    end = data["coopPlayer2"] + data["sizeofCoopPlayer2"]
    if end > data["sizeofSaveBlock1"]:
        print(f"error: coopPlayer2 ends at {end}, past SaveBlock1's "
              f"{data['sizeofSaveBlock1']} bytes.", file=sys.stderr)
        return 1

    if not ELF.exists():
        print(f"error: {ELF} not found. Run `make modern` first.", file=sys.stderr)
        return 1

    addrs = ewram_symbols()
    missing = sorted(set(WANTED_SYMBOLS.values()) - set(addrs))
    if missing:
        print(f"error: {', '.join(missing)} not found in {ELF}. Were the "
              "co-op sources renamed or optimised out?", file=sys.stderr)
        return 1

    for key, addr in addrs.items():
        if not (0x02000000 <= addr < 0x02040000):
            print(f"error: {key} is 0x{addr:08X}, outside EWRAM. The harness "
                  "addresses memory relative to the mailbox and cannot reach "
                  "it.", file=sys.stderr)
            return 1

    data.update(addrs)

    if data["flags"] == 0 or data["dexSeen"] == 0:
        print("error: flags/dexSeen resolved to offset 0, which is pos. "
              "The arrays were almost certainly read in the wrong order.",
              file=sys.stderr)
        return 1

    data["_comment"] = "Generated by tools/coop/emit_offsets.py. Do not edit."
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")

    print(f"wrote {out_path}: coopPlayer2 at {data['coopPlayer2']}, "
          f"flags at {data['flags']}, dexSeen at {data['dexSeen']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
