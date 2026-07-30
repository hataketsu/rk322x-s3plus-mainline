/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rk-ftl.h - Rockchip rknand read-only FTL (Phase B) for mainline MTD
 *
 * Reimplements the vendor rknand logical->physical (L2P) translation on top of
 * the mainline rockchip-nand-controller MTD raw-NAND driver, so the vendor
 * NAND's logical rootfs can be exposed as a read-only block device.
 *
 * Reference (authoritative): docs/rknand-re/ftl-l2p-algorithm.md,
 * ftl-oob-format.md, nand-info-struct.md, and tools/l2p-scan.py.
 *
 * READ-ONLY. No write/GC path here - that is a later phase.
 */
#ifndef __RK_FTL_H__
#define __RK_FTL_H__

#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/blktrans.h>

/* ---- Geometry (verified, see nand-info-struct.md / ftl-l2p-algorithm.md) ---- */
#define RK_FTL_SECT_SIZE	512			/* host sector, block-layer unit */
#define RK_FTL_SPP		16			/* sectors per page (spp)        */
#define RK_FTL_PAGE_SIZE	(RK_FTL_SPP << 9)	/* 8192 host bytes / page        */
#define RK_FTL_STEPS		8			/* BCH steps per page (sys words)*/

/* u32 PPA entries per L2P region == one map page's payload / 4 == spp * 128 */
#define RK_FTL_ENTRIES_PER_REGION	(RK_FTL_SPP * 128)	/* 2048 */

/* ---- Per-page OOB metadata magics (ftl-oob-format.md) ---- */
#define RK_FTL_MAGIC_DATA	0xF095	/* normal data page (route A key)     */
#define RK_FTL_MAGIC_SYSINFO	0xF0A4	/* sys-info block page-0              */
#define RK_FTL_MAGIC_MAP	0xF0C2	/* L2P map page (route B payload)     */
#define RK_FTL_MAGIC_EXT	0xF086	/* ext block                          */
#define RK_FTL_MAGIC_ERASED	0xFFFF	/* erased / free                      */
#define RK_FTL_MAGIC_LOG	0xFAF5	/* log-snapshot page (skip in map blk)*/

/* Invalid / sentinel PPA (also erased u32) */
#define RK_FTL_PPA_INVALID	0xFFFFFFFFu
#define RK_FTL_VER_INVALID	0xFFFFFFFFu

/* ---- PPA decode (ftl-oob-format.md) ----
 *   blk  = (ppa & 0x3FFFFFF) >> 10
 *   page =  ppa & 0x3FF
 *   bit31 = SLC-mode flag
 */
#define RK_FTL_PPA_PAGE(ppa)	((ppa) & 0x3FF)
#define RK_FTL_PPA_BLK(ppa)	(((ppa) & 0x03FFFFFFu) >> 10)
#define RK_FTL_PPA_IS_SLC(ppa)	(((ppa) >> 31) & 1u)

/*
 * Version comparison (wrapping 32-bit counter, skips 0xFFFFFFFF).
 * "a is newer than b" iff ((a - b) & 0xFFFFFFFF) < 0x80000001.
 */
static inline bool rk_ftl_ver_newer(u32 a, u32 b)
{
	return ((a - b) & 0xFFFFFFFFu) < 0x80000001u;
}

/*
 * De-rotation of the mainline-exposed sys bytes.
 * rockchip-nand-controller places BCH step 'i' sys word at OOB word position
 * (i == 0) ? STEPS-1 : i-1.  So to recover vendor step 's' sys word, read OOB
 * word at position rk_ftl_sysword_pos(s).  step0 (magic) lands at word 7.
 */
static inline int rk_ftl_sysword_pos(int step)
{
	return (step == 0) ? (RK_FTL_STEPS - 1) : (step - 1);
}

/* Decoded 16-byte vendor OOB metadata struct (de-rotated). */
struct rk_ftl_oob {
	u16 magic;	/* +0x00 */
	u16 tag;	/* +0x02 block / superblock tag */
	u32 version;	/* +0x04 monotonic sequence     */
	u32 lpn;	/* +0x08 logical page number (route A key) */
	u32 prev_ppa;	/* +0x0C previous PPA (GC/recovery); on MAP pages this
			 *       word position instead holds region (+0x08) and
			 *       checksum (+0x0C) - see ftl-l2p-algorithm.md */
};

/*
 * Sys-info (0xF0A4) decoded header. Only 'capacity' is load-bearing for the
 * read path today; open-superblock ids + max_version drive the overlay pass.
 * Exact byte offsets of the 0x30 header are a TODO (FtlLoadSysInfo b097c3ec).
 */
struct rk_ftl_sysinfo {
	u32 capacity;		/* total logical pages (FtlGetLpn) */
	u32 max_version;	/* newest version at flush time    */
	u32 open_sblk[2];	/* open superblock ids: normal + SLC/GC (TODO) */
	int num_open_sblk;
};

/* Per-instance FTL context. */
struct rk_ftl {
	struct mtd_blktrans_dev mbd;	/* must be usable via container_of */
	struct mtd_info *mtd;

	/* Geometry derived from mtd at add time. */
	u32 page_size;			/* == RK_FTL_PAGE_SIZE (writesize) */
	u32 pages_per_block;		/* mtd->erasesize / mtd->writesize */
	u32 total_blocks;		/* mtd->size / mtd->erasesize      */
	u64 total_pages;		/* mtd->size / mtd->writesize      */
	u32 sysblk_num;			/* trailing block-rows to scan     */

	/* Logical geometry. */
	struct rk_ftl_sysinfo sys;
	u32 capacity;			/* total LPNs (== sys.capacity)    */

	/* Level-1 map: region -> PPA of that region's map page. */
	u32 *region_to_ppa;		/* [num_regions] */
	u32 num_regions;

	/* Expanded L2P table: L2P[lpn] = PPA. ~capacity u32 (~3.8 MB). */
	u32 *l2p;			/* vmalloc'd, [capacity] */

	/* Scratch page buffer for readsect (protected by mbd.lock). */
	u8 *page_buf;			/* page_size bytes */
	u32 page_buf_lpn;		/* which LPN currently cached, or ~0 */

	/* Scratch OOB buffer, mtd->oobsize bytes. */
	u8 *oob_buf;
};

static inline struct rk_ftl *mbd_to_ftl(struct mtd_blktrans_dev *dev)
{
	return container_of(dev, struct rk_ftl, mbd);
}

#endif /* __RK_FTL_H__ */
