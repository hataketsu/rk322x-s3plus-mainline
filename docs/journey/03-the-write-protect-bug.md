# 03 — The bug that ate three days

The symptom, from the vendor FTL's own provisioning pass:

```
prog read s error: = 0 a5a5a5a5 ffffffff
prog read d error: = 0 5a5a5a5a ffffffff
E:bad block:0,1,2,3,4,...
```

`FlashMakeFactorBbt` writes a test pattern to a block, reads it back, gets
`0xffffffff`, and marks the block bad. For all 4096 blocks. The console flood at
default loglevel then made the box look hung, which sent the investigation down a
"the driver wedges the kernel" path for a while before serial console was working.

The chip reported **ready**, the status register said **PASS**, and the cells were
unchanged. That combination — a program that succeeds and does nothing — is worth
remembering, because it has a short list of causes and none of them are software.

## Everything that was wrong with the theories

This is the useful part of the story. Every one of these was plausible, several
were investigated for hours, and all were wrong:

| theory | how it died |
|---|---|
| The kernel-side DMA helpers don't flush caches before the program | `dma_map_single(..., DMA_TO_DEVICE)` is exactly the cache-clean call; the mapping and direction were correct, and buffers were `kmalloc(GFP_DMA)` |
| The blob's wait helpers return early | Disassembly: FTL-v5's write path never calls the kernel wait shims at all. `NandcWaitFlashReady` self-polls FMCTL bit 9; `NandcXferComp`'s write branch self-polls FLCTL bit 20 |
| The FLCTL `XFER_READY` poll races | Real, but harmless. Instrumentation proved bit 20 *is* already set before a transfer starts — it is the register's reset value, `0x00100000` — yet the identical pattern exists in the working vendor 4.4 driver |
| The write buffer contains stale `0xFF` | Logging the buffer at DMA-map time showed real data (`4e414e44`, a "NAND" magic) |
| Vendor SLC-mode commands confuse the chip | FTL-v5 does send Micron `SET FEATURE 0xEF/0x91` + `0xDA` when `gFlashSlcMode` is set. Patched the blob to force it off — **no change**. Reverted |
| `NandcTimeCfg` produces garbage timings | Blob and mainline both end up at `FMWAIT = 0x1061` |
| Wrong ECC/randomizer for the write path | Would corrupt data, not leave cells erased |

A lot of that effort went into reading 500 KB of generated assembly to disprove
software theories. It was not wasted — the notes are what made later work fast —
but the bug was never in software.

## The measurement that ended it

Two facts, gathered instead of reasoned about.

**First: what is actually on the media?** After a full failing provisioning pass,
the flash was read back with the *mainline* driver — a completely independent path
from the FTL. Result: every block still erased. Blocks 0–7 read clean `0xFF`
(mainline leaves the randomizer off for boot blocks) and everything above read the
descrambled-blank signature `2c99d86e7138bdde`.

That single observation kills an entire class of theory. If the FTL had programmed
*anything* — even a buffer full of `0xFF` — the randomizer would have scrambled it
and the cells would hold `scramble(0xFF)`, which is not raw `0xFF`. Nothing had
ever been programmed. So the fault was below the FTL.

**Second: can anything write this chip?** The mainline driver was asked to erase
and write directly. It failed too:

```
MEMERASE   → -EIO
page write → -EIO
```

That was decisive. Two unrelated drivers, one closed and one upstream, both
unable to modify the flash while both read it perfectly. The problem was the
board.

## The cause

`/proc/device-tree/pinctrl/flash/` lists eleven pin groups. The device-tree
overlay enabling the NAND controller listed eight of them:

```
pinctrl-0 = <ale bus8 cle cs0 dqs rdn rdy wrn>;
```

Missing: **`flash-wp`**. The write-protect pin was never muxed to the controller,
so WP# stayed asserted, and the chip ignored every PROGRAM and ERASE while serving
reads normally.

The fix is one phandle:

```
pinctrl-0 = <0xa7 0xab 0xa8 0xa2 0xac 0xaa 0xa6 0xa9 0xad>;
                                                     ^^^^ flash-wp
```

## Why it hid so well

Because **`FMCTL` bit 8 reads back as 1 either way.**

That bit is the controller's WP# output level — mainline calls it `FMCTL_WP` and
sets it once in `rk_nfc_hw_init()`. It reports what the controller is driving, not
whether that signal reaches the chip. With the pin unmuxed, the register says
"write protect disabled" and the pad is still disconnected.

So every register-level check said the hardware was healthy. Reads worked, which
made the setup look correct. And the vendor FTL made it worse than the mainline
driver did: mainline returns a clean `-EIO`, while the FTL's programs return
status PASS, so the failure surfaced as "readback mismatch" — which reads like a
data-path or ECC bug, not a board bug.

## The controlled experiment

Correlation is not causation, and the fix arrived alongside a reboot. So it was
tested properly: same block, same driver, one phandle as the only variable.

| | `flash-wp` | FMCTL | erase | write |
|---|---|---|---|---|
| A | absent | `0x00000701` | **-EIO** | **-EIO** |
| B | present | `0x00000701` | OK | OK, readback byte-exact |

`FMCTL` is **identical** in both phases. That is the whole lesson in one row: the
controller register is never the tell — only the pin mux is.

## After the fix

The vendor FTL provisioned the blank chip in about four seconds with zero
`prog read` / `prog error` / `bad block` lines, and produced:

```
/dev/rknand0   15704064 sectors = 7668 MB
mkfs.ext4 + mount → 7.3 G
64 MB urandom write at 5.9 MB/s, md5 identical after umount/remount and reboot
```

## What to take from it

- **A pinctrl group missing from a device-tree node produces a symptom in the
  peripheral, not in the device tree.** Nothing logs "you forgot a pin".
- **Reads working is not evidence the pinmux is complete.** Data, address and
  control pins can all be muxed while a control signal like WP# is not.
- **When two independent drivers fail the same way, stop debugging drivers.**
  That test was available on day one and would have saved most of the time.
- **Read the media with a second, independent path.** The FTL's own readback said
  "0xffffffff" and was believed for days. The mainline driver's read of the same
  blocks answered a different and better question: *has anything ever been written
  here?*
- **Note which pin groups exist, not just which ones you used.** One `ls` of
  `/proc/device-tree/pinctrl/<controller>/` was the whole answer.
