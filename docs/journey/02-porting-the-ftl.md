# 02 — Porting the closed FTL to kernel 6.6

Reading the flash was solved; writing it safely was not. Reimplementing a flash
translation layer means reimplementing wear levelling, garbage collection,
bad-block management and power-fail safety on an MLC part — weeks of work with
many ways to silently destroy data.

The alternative: take Rockchip's own FTL and make it run on 6.6.

## What "the FTL" actually is

Rockchip ships the FTL as **GCC-emitted assembly**, not source:

```
drivers/rk_nand/rk_ftlv5_arm32.S     469 KB     FTL v5   (CamelCase API)
drivers/rk_nand/rk_zftl_arm32.S      644 KB     ZFTL     (lowercase API)
drivers/rk_nand/rk_nand_blk.c        ~700 lines block driver glue
drivers/rk_nand/rk_nand_base.c       ~456 lines NFC init + kernel helpers
```

The `.S` files are the deliverable — there is no C to recompile. They are
publicly distributed in Rockchip's BSP and in vendor SDKs, so this is not a
redistribution problem, but it does mean the blob's ABI is fixed and *we* have to
adapt around it.

Getting the right source tree mattered. Three candidates exist and only one is
correct for this box:

| tree | verdict |
|---|---|
| `drivers/mtd/rknand` | old shim, not this |
| `drivers/rkflash` | SLC-only, not this |
| **`drivers/rk_nand`** (RK 5.10 BSP) | correct — has the v5 blob matching the box's FTL 5.0.5x |

Both `rk_zftl_arm32.o` and `rk_ftlv5_arm32.o` get linked. `rk_ftl_init` enters
through ZFTL, whose `nand_flash_init` rejects this MLC part and falls through to
FTL-v5's `FlashInit`. So the code that actually runs on this box is the **v5,
CamelCase** half — worth knowing, because chasing the lowercase functions is a
detour.

## The port itself

Mostly a 5.10 → 6.6 block-layer migration:

| 5.10 | 6.6 |
|---|---|
| `alloc_disk()` | `blk_mq_alloc_disk()` |
| `blk_mq_init_sq_queue()` | gone — the queue comes from `blk_mq_alloc_disk()` |
| `blk_cleanup_queue()` | `blk_mq_destroy_queue()` |
| `GENHD_FL_EXT_DEVT` | removed; default flags keep partition scanning |
| `PDE_DATA()` | `pde_data()` |

Plus shims for symbols the blob references that 6.6 no longer exports as
functions:

```c
/* printk() is a macro in 6.6; the blob calls the symbol */
asmlinkage int printk(const char *fmt, ...) { ... vprintk(fmt, args); ... }

/* usleep_range() became a static inline over usleep_range_state() */
void usleep_range(unsigned long min, unsigned long max)
{ usleep_range_state(min, max, TASK_UNINTERRUPTIBLE); }

/* the blob was built with unwind tables; provide empty personality routines */
int __aeabi_unwind_cpp_pr0(void) { return 0; }
```

Small but real fixes along the way:

- **Do not hardcode block major 31.** It collides with `mtdblock`, giving
  `register_blkdev: cannot get major 31`. Use a dynamic major, and unwind
  properly on failure — leaving the platform driver registered while the module
  tears down leaves the driver core pointing into freed module text.
- **Deregister the misc devices** (`rknand_sys_storage`, `vendor_storage`) on
  exit, or a reload oopses.

## Provisioning a blank NAND

The port loaded, detected the chip (`No.1 FLASH ID: 2c 64 44 4b a9`), printed
`FTL version: 5.0.63 20210616` — and then `rk_ftl_get_capacity()` returned **0**,
so no block device appeared.

The capacity (`DeviceCapacity`, blob context + `0xf44`) is set in exactly two
places: `FtlLoadSysInfo`, which reads existing provisioning, and `FtlLowFormat`,
which creates it. On a blank device both the BBT load and `FtlSysBlkInit(0)`
fail, and — this is the important part — `FtlInit` then takes a branch that only
*prints* "no sys info" and returns 0. **It does not provision.**

Calling `FtlLowFormat()` directly oopses at `FtlBbmIsBadBlock+0x40` with a NULL
per-region bad-block bitmap: it depends on `FlashMakeFactorBbt` having run first.
The blank-device path is effectively untested in the kernel FTL because on real
vendor hardware the **miniloader provisions the flash over USB** and the kernel
FTL only ever attaches to already-provisioned media.

The exported entry point that does the whole sequence in the right order is
`FtlReInitForSDUpdata()`. The driver now runs, in a kthread so a slow or wedged
provisioning cannot block `insmod`:

```
capacity == 0 ?
    FtlReInitForSDUpdata()
    fallback: FlashLoadFactorBbt / FlashMakeFactorBbt + FtlLowFormat
    → nand_add_dev()
    → start GC thread, procfs, storage ioctls
```

## What it gives you

```
/dev/rknand0        15704064 sectors = 7668 MB
/proc/rknand        FTL version, capacity, LPN counts, GC stats, bad block count
```

A normal block device with wear levelling and bad-block management, partitionable,
`mkfs`-able, and — because the *same blob* is linked into the vendor U-Boot —
byte-identical in layout to what U-Boot sees. That last property is what makes
the boot chain in [article 04](04-uboot.md) straightforward.

Two things about the FTL are worth knowing before you rely on it:

- **The `/dev/rknand_sys_storage` ioctl interface is version-dependent.** On FTL
  5.0.63 `rknand_sys_storage_ioctl` accepts exactly one command, `0x40047254`
  (flush), and returns `-EINVAL` for everything else. The `WRITE_SECTOR` /
  `END_WRITE` ioctls documented from the 4.4 blob simply do not exist here. The
  idblock is written a different way — see [article 04](04-uboot.md).
- **`FlashMakeFactorBbt` tests every block by erase + program + verify.** If
  writes are broken for any reason, it will dutifully mark all 4096 blocks bad and
  flood the console doing it. Which is exactly what happened next:
  [article 03](03-the-write-protect-bug.md).
