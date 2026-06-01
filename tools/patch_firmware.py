#!/usr/bin/env python3
"""
Patch the Qingping PM sensor firmware (HT32F52243, V1.0.03.230201) so that
the sensor boots into USART1 mode on every power-up, no SWD attached.

Patch summary
-------------
The stock firmware has a startup mode selector at *pcVar3 = SRAM 0x20000038
that picks between USART1 (mode 1) and bitbang (mode 0).  The BSS default
is 0, and NOTHING in the binary writes to it.  Therefore the production
firmware boots in mode 0 (bitbang), which has no working TX path on PA4.

FUN_00002d88 (called early in startup, before the mode check) does:
    flash_read(0xC400, 1, buf=0x200000C0);
    if (*0x200000C0 != 0xFF)
        *(*0x00002e08) = byte;     # store config byte through pointer at flash 0x2E08

Currently the pointer at flash 0x2E08 = 0x200000B4 (pcVar3[0x7C], an
auto-output flag that's only read inside the command parser — useless in
mode 0).

By redirecting that pointer to 0x20000038 we re-purpose the existing
"persistent auto-output" config byte at flash 0xC400 to instead drive the
mode selector.  Writing 0xC400 = 0x01 then makes every boot land in mode 1:
USART1 hardware initialized on PA4(TX)/PA5(RX), 9600 8N1, periodic 32-byte
BM frames stream out on PA4 once the sensor stabilizes.

Only 4 bytes of code change, plus 1 byte of config.  pcVar3[0x7C] is still
writable via the BM 0x20 command (different pointer at flash 0x3698), so
nothing else breaks.

NB: PA13 (SWDIO) has the HT32's internal pull-up, so the SWD-attach trap
at 0x426C is harmless when no debugger is actively pulling PA13 low.
"""
import sys, hashlib, pathlib

REPO = pathlib.Path(__file__).resolve().parent.parent
FW_DIR = REPO / "firmware" / "V1.0.03.230201"
SRC = FW_DIR / "ht32_flash_64k.bin"
DST = FW_DIR / "ht32_flash_64k.patched.bin"

EXPECTED_MD5 = "73b304332c4bc9b9817f764ca616a10b"

PATCHES = [
    # (offset, expected_bytes, new_bytes, description)
    (0x2E08, bytes.fromhex("b4000020"), bytes.fromhex("38000020"),
     "Redirect FUN_00002d88's STRB to pcVar3[0] (mode selector)"),
    # Unfilter patches: turn the 7-window boxcar smoother in FUN_00004154 into
    # a pass-through so the BM frame carries the raw most-recent 1-second
    # window's bin counts.  Two adjacent halfword edits in flash page 0x4400:
    #   0x4570: ldrh r3,[r3,#0x1e] -> ldrb r3,[r3,#0x0d]   (iter starts at d)
    #   0x45E8: cmp  r3,#0x7       -> cmp  r3,#0x1         (loop runs once)
    (0x4570, bytes.fromhex("db8b"),     bytes.fromhex("5b7b"),
     "Unfilter: read history[d] instead of history[state[0x1e]]"),
    (0x45E8, bytes.fromhex("072b"),     bytes.fromhex("012b"),
     "Unfilter: run the running-sum loop body exactly once"),
    # Histogram mode: invert the state[0x12]==0 gate at 0x460E so that
    # FUN_00004154 takes the raw-histogram branch (block at 0x48F2 that
    # transmits a 32-byte BM frame containing running_sum[1..11]) instead
    # of the µg/m³ pipeline that ends in FUN_00001c5c (Plantower-style
    # frame).  state[0x12] is hardcoded to 1 at boot and never cleared,
    # so the conditional is in practice a never-taken branch; flipping
    # 'beq' to unconditional 'b' (same imm8=0x7D offset) makes it
    # always-taken.
    (0x460E, bytes.fromhex("7dd0"),     bytes.fromhex("7de0"),
     "Histogram mode: force the BM-emit gate to the raw-histogram branch"),
    (0xC400, bytes([0xFF]),             bytes([0x01]),
     "Enable mode 1 (USART1) on boot"),
]

def main():
    data = bytearray(SRC.read_bytes())
    assert len(data) == 0x10000, f"unexpected size {len(data)}"
    md5 = hashlib.md5(bytes(data)).hexdigest()
    if md5 != EXPECTED_MD5:
        print(f"WARNING: source MD5 {md5} != expected {EXPECTED_MD5}")
    for off, expect, new, desc in PATCHES:
        cur = bytes(data[off:off+len(expect)])
        if cur != expect:
            print(f"FAIL @ 0x{off:04X}: have {cur.hex()} expected {expect.hex()}")
            sys.exit(1)
        data[off:off+len(new)] = new
        print(f"OK   @ 0x{off:04X}: {expect.hex()} -> {new.hex()}  ({desc})")
    DST.write_bytes(bytes(data))
    print(f"\nwrote {DST}  md5={hashlib.md5(bytes(data)).hexdigest()}")

if __name__ == "__main__":
    main()
