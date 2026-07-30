// SPDX-License-Identifier: GPL-2.0
/*
 * rk-ftl.c - Rockchip rknand read-only FTL (Phase B) as an MTD blktrans layer.
 *
 * Sits ABOVE the mainline rockchip-nand-controller MTD driver (which already
 * does randomizer descramble + NAND_SKIP_BBTSCAN and exposes raw descrambled
 * 8192-byte pages + rotated 16-byte OOB via mtd_read / mtd_read_oob).  This
 * module reconstructs the vendor logical->physical map and registers a
 * read-only block device (/dev/rkftlX) that serves the vendor logical space.
 *
 * Chosen block-layer approach: MTD blktrans (drivers/mtd/mtd_blkdevs.c). It is
 * the natural fit - it wires a translation layer with a per-sector readsect()
 * callback straight onto an mtd_info, gives us readonly-by-omission (no
 * writesect => the block device is RO), and mirrors mtdblock / ftl.c / inftl.
 * See docs/rknand-re/phase-b-ftl-read-design.md for the full rationale.
 *
 * Algorithm reference (authoritative): docs/rknand-re/ftl-l2p-algorithm.md,
 * ftl-oob-format.md, nand-info-struct.md; reference impl tools/l2p-scan.py.
 *
 * READ-ONLY.  Do NOT add writesect/GC here - separate phase.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/blktrans.h>

#include "rk-ftl.h"

#define RK_FTL_NAME	"rkftl"

/* ------------------------------------------------------------------ */
/* Low-level MTD helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Read the raw (descrambled) OOB for one physical page and decode the
 * de-rotated 16-byte vendor metadata struct.  Mirrors sw()/de-rotation in
 * tools/l2p-scan.py.  Uses MTD_OPS_PLACE_OOB (mode 0) so we get exactly what
 * the driver's read path (and the MEMREADOOB ioctl used by l2p-scan.py)
 * produces: descrambled, ECC-processed, sys-bytes rotated.
 *
 * @phys_page: linear physical page index (offset = phys_page * page_size).
 */
static int rk_ftl_read_oob(struct rk_ftl *ftl, u64 phys_page,
			   struct rk_ftl_oob *out)
{
	struct mtd_info *mtd = ftl->mtd;
	struct mtd_oob_ops ops = { };
	loff_t off = (loff_t)phys_page * ftl->page_size;
	u32 w[4];
	int i, ret;

	ops.mode = MTD_OPS_PLACE_OOB;
	ops.ooboffs = 0;
	ops.ooblen = mtd->oobsize;
	ops.oobbuf = ftl->oob_buf;
	ops.datbuf = NULL;

	ret = mtd_read_oob(mtd, off, &ops);
	/*
	 * -EUCLEAN (corrected bitflips) and -EBADMSG (uncorrectable) are not
	 *  fatal for a scan: an erased/garbage page just fails the magic test.
	 *  Treat only hard errors as fatal.
	 */
	if (ret && ret != -EUCLEAN && ret != -EBADMSG)
		return ret;

	/* De-rotate: vendor step s sys word lives at OOB word rk_ftl_sysword_pos(s). */
	for (i = 0; i < 4; i++) {
		int pos = rk_ftl_sysword_pos(i);

		w[i] = le32_to_cpup((__le32 *)(ftl->oob_buf + pos * 4));
	}

	out->magic    = w[0] & 0xFFFF;
	out->tag      = w[0] >> 16;
	out->version  = w[1];
	out->lpn      = w[2];
	out->prev_ppa = w[3];
	return 0;
}

/*
 * Read one full descrambled data page (8192 B) by linear physical page index.
 * Uses the normal ECC read path (mtd_read), exactly like os.pread on /dev/mtdN
 * in tools/l2p-extract.py.
 */
static int rk_ftl_read_page(struct rk_ftl *ftl, u64 phys_page, u8 *buf)
{
	struct mtd_info *mtd = ftl->mtd;
	loff_t off = (loff_t)phys_page * ftl->page_size;
	size_t retlen = 0;
	int ret;

	ret = mtd_read(mtd, off, ftl->page_size, &retlen, buf);
	if (ret == -EUCLEAN)
		ret = 0;			/* corrected, data is good */
	if (ret)
		return ret;
	if (retlen != ftl->page_size)
		return -EIO;
	return 0;
}

/* Convert a decoded PPA into a linear physical page index. */
static u64 rk_ftl_ppa_to_linear(struct rk_ftl *ftl, u32 ppa)
{
	u32 blk = RK_FTL_PPA_BLK(ppa);
	u32 page = RK_FTL_PPA_PAGE(ppa);

	/*
	 * TODO(plane/superblock): the vendor groups blocks into multi-plane
	 * superblocks; 'blk' here is a physical block index in that scheme.
	 * For a single-plane linear device this is simply:
	 *     linear = blk * pages_per_block + page.
	 * If the box turns out to use N-plane superblocks the mapping becomes
	 *     linear = (blk * planes + plane) * pages_per_block + page.
	 * Verify against tools/l2p-scan.py route-A PPAs (which store the linear
	 * page directly) before trusting reads near block boundaries.
	 * Ref: ftl-l2p-algorithm.md "PPA decode" + nand-info-struct.md geometry.
	 */
	return (u64)blk * ftl->pages_per_block + page;
}

/* ------------------------------------------------------------------ */
/* Boot map-load path (route B).  Maps 1:1 onto tools/l2p-scan.py and     */
/* the vendor FtlSysBlkInit sequence in ftl-l2p-algorithm.md.            */
/* ------------------------------------------------------------------ */

/*
 * Step 1 - FtlScanSysBlk (b097aef4): scan the trailing sysblk_num block-rows,
 * read each non-bad block's page-0 OOB, bucket by magic. Populates
 * region_to_ppa collection inputs and locates the newest sys-info block.
 *
 * TODO: full implementation. Needs:
 *   - iterate blk in [total_blocks - sysblk_num, total_blocks)
 *   - skip bad blocks (mtd_block_isbad)
 *   - read page-0 OOB (rk_ftl_read_oob at blk*pages_per_block)
 *   - collect 0xF0A4 (keep newest 2 by version), 0xF086, 0xF0C2 (all, sorted)
 * Ref: ftl-l2p-algorithm.md route B step 1.
 */
static int rk_ftl_scan_sysblk(struct rk_ftl *ftl)
{
	/* sysblk_num = max(param, 24); we have no param block parsed yet. */
	ftl->sysblk_num = 24;	/* TODO: read real param, use max(param, 24) */

	/* TODO: the scan loop described above. */
	dev_info(&ftl->mtd->dev,
		 "rkftl: TODO scan_sysblk over trailing %u block-rows\n",
		 ftl->sysblk_num);
	return 0;
}

/*
 * Step 2 - FtlLoadSysInfo (b097c3ec): from the newest 0xF0A4 block, read the
 * last valid page (magic + DATA[0..3] signature) and parse the 0x30 header to
 * get capacity (total LPNs), open-superblock ids and max_version.
 *
 * TODO: exact header field offsets from RE (b097c3ec). For now capacity is
 * derived from device size as a placeholder so the rest of the path is
 * exercisable; MUST be replaced with the sys-info value before trusting the
 * logical size (the logical rootfs is smaller than the raw device).
 */
static int rk_ftl_load_sysinfo(struct rk_ftl *ftl)
{
	/* TODO: locate newest 0xF0A4 block, read last valid page, parse header. */

	/* Placeholder capacity: whole device in logical pages. */
	ftl->sys.capacity = (u32)ftl->total_pages;
	ftl->sys.max_version = 0;
	ftl->sys.num_open_sblk = 0;
	ftl->capacity = ftl->sys.capacity;

	dev_info(&ftl->mtd->dev,
		 "rkftl: TODO load_sysinfo - using placeholder capacity=%u LPNs\n",
		 ftl->capacity);
	return 0;
}

/*
 * Step 3 - FtlMapTblRecovery (b097bca8): build the level-1 region_to_ppa table.
 * Process map blocks in ascending version; for each 0xF0C2 map page (skip the
 * 0xFAF5 log-snapshot page), region_to_ppa[region] = PPA(page) (later wins).
 * region number is at the page OOB +0x08 (== prev_ppa word position here).
 *
 * TODO: the map-block walk. Allocation of region_to_ppa is done here.
 * Ref: ftl-l2p-algorithm.md route B step 3.
 */
static int rk_ftl_map_recovery(struct rk_ftl *ftl)
{
	u32 i;

	ftl->num_regions = DIV_ROUND_UP(ftl->capacity, RK_FTL_ENTRIES_PER_REGION);
	ftl->region_to_ppa = kmalloc_array(ftl->num_regions, sizeof(u32),
					   GFP_KERNEL);
	if (!ftl->region_to_ppa)
		return -ENOMEM;
	for (i = 0; i < ftl->num_regions; i++)
		ftl->region_to_ppa[i] = RK_FTL_PPA_INVALID;

	/*
	 * TODO: walk map blocks (from scan_sysblk) in ascending version; for
	 * every 0xF0C2 page whose OOB is not 0xFAF5, decode region = OOB+0x08
	 * and set region_to_ppa[region] = PPA(that page). Later version wins,
	 * so process in ascending order and let later overwrite earlier.
	 */
	dev_info(&ftl->mtd->dev,
		 "rkftl: TODO map_recovery - %u regions (epr=%u)\n",
		 ftl->num_regions, RK_FTL_ENTRIES_PER_REGION);
	return 0;
}

/*
 * Step 4 - Expand region_to_ppa into the flat L2P[capacity] table.
 * For each region with a valid map-page PPA, read that map page's u32 LE PPA
 * array and copy into L2P[region*epr + i] (skipping 0xFFFFFFFF entries).
 * Ref: ftl-l2p-algorithm.md route B step 4.
 */
static int rk_ftl_expand_l2p(struct rk_ftl *ftl)
{
	u32 region;
	__le32 *map_words;
	int ret = 0;

	/* L2P is large (~capacity u32 ~= 3.8 MB) - vmalloc, zeroed to INVALID. */
	ftl->l2p = vmalloc(array_size(ftl->capacity, sizeof(u32)));
	if (!ftl->l2p)
		return -ENOMEM;
	memset(ftl->l2p, 0xFF, array_size(ftl->capacity, sizeof(u32)));

	map_words = (__le32 *)ftl->page_buf;	/* reuse page scratch buffer */

	for (region = 0; region < ftl->num_regions; region++) {
		u32 ppa = ftl->region_to_ppa[region];
		u64 base = (u64)region * RK_FTL_ENTRIES_PER_REGION;
		u32 i, n;

		if (ppa == RK_FTL_PPA_INVALID)
			continue;

		ret = rk_ftl_read_page(ftl, rk_ftl_ppa_to_linear(ftl, ppa),
				       ftl->page_buf);
		if (ret) {
			dev_warn(&ftl->mtd->dev,
				 "rkftl: map page read failed region=%u ret=%d\n",
				 region, ret);
			ret = 0;	/* leave region as holes, keep going */
			continue;
		}

		n = min_t(u32, RK_FTL_ENTRIES_PER_REGION,
			  ftl->capacity - (u32)base);
		for (i = 0; i < n; i++) {
			u32 entry = le32_to_cpu(map_words[i]);

			if (entry != RK_FTL_PPA_INVALID)
				ftl->l2p[base + i] = entry;
		}
	}

	ftl->page_buf_lpn = ~0u;	/* invalidate scratch cache after reuse */
	return ret;
}

/*
 * Step 5 - Overlay open superblocks (ftl_open_sblk_recovery): scan the open
 * data superblock(s) page-by-page in program order; for each valid data page
 * whose version beats the current mapping for its LPN, set L2P[LPN]=PPA(page).
 * Open writes are post-flush and override the bulk map.
 *
 * TODO: needs open-superblock ids from sys-info (step 2). Structurally this is
 * a per-page rk_ftl_read_oob loop with rk_ftl_ver_newer() gating, exactly like
 * the route-A collision rule in tools/l2p-scan.py.
 * Ref: ftl-l2p-algorithm.md route B step 5.
 */
static int rk_ftl_overlay_open(struct rk_ftl *ftl)
{
	/*
	 * TODO: for each open superblock id in ftl->sys.open_sblk[]:
	 *   for each page P in program order:
	 *     decode OOB; if magic==DATA and version!=INVALID:
	 *       if L2P[lpn]==INVALID or ver_newer(oob.version, ver_of(L2P[lpn]))
	 *         L2P[lpn] = PPA(P)
	 * We do not currently store a per-LPN version array; the overlay needs
	 * one (or must re-read the incumbent page's OOB) to resolve precedence.
	 */
	dev_info(&ftl->mtd->dev, "rkftl: TODO overlay_open (%d open sblk)\n",
		 ftl->sys.num_open_sblk);
	return 0;
}

/* Full boot map-load sequence (FtlSysBlkInit order). */
static int rk_ftl_build_map(struct rk_ftl *ftl)
{
	int ret;

	ret = rk_ftl_scan_sysblk(ftl);
	if (ret)
		return ret;
	ret = rk_ftl_load_sysinfo(ftl);
	if (ret)
		return ret;
	ret = rk_ftl_map_recovery(ftl);
	if (ret)
		return ret;
	ret = rk_ftl_expand_l2p(ftl);
	if (ret)
		return ret;
	ret = rk_ftl_overlay_open(ftl);
	if (ret)
		return ret;

	dev_info(&ftl->mtd->dev,
		 "rkftl: L2P built, capacity=%u LPNs (~%llu MB logical)\n",
		 ftl->capacity,
		 (unsigned long long)ftl->capacity * RK_FTL_PAGE_SIZE >> 20);
	return 0;
}

/* ------------------------------------------------------------------ */
/* blktrans ops                                                        */
/* ------------------------------------------------------------------ */

/*
 * Serve one 512-byte host sector. block = host sector index.
 *   LPN         = block / spp
 *   sec_in_page = block % spp
 * We read the whole 8192-byte physical page (cached across the 16 sectors of
 * one LPN) and copy out the requested 512-byte slice. Unmapped LPNs read as
 * zeros - matching the hole behaviour of tools/l2p-extract.py.
 */
static int rk_ftl_readsect(struct mtd_blktrans_dev *dev,
			   unsigned long block, char *buf)
{
	struct rk_ftl *ftl = mbd_to_ftl(dev);
	u32 lpn = block / RK_FTL_SPP;
	u32 sec = block % RK_FTL_SPP;
	u32 ppa;
	int ret;

	if (lpn >= ftl->capacity) {
		memset(buf, 0, RK_FTL_SECT_SIZE);
		return 0;
	}

	/* Fast path: sector belongs to the page we last decoded. */
	if (ftl->page_buf_lpn != lpn) {
		ppa = ftl->l2p[lpn];
		if (ppa == RK_FTL_PPA_INVALID) {
			/* hole: zero-fill this and remember it's a hole page */
			memset(ftl->page_buf, 0, ftl->page_size);
		} else {
			ret = rk_ftl_read_page(ftl,
					       rk_ftl_ppa_to_linear(ftl, ppa),
					       ftl->page_buf);
			if (ret)
				return ret;
		}
		ftl->page_buf_lpn = lpn;
	}

	memcpy(buf, ftl->page_buf + sec * RK_FTL_SECT_SIZE, RK_FTL_SECT_SIZE);
	return 0;
}

static int rk_ftl_open(struct mtd_blktrans_dev *dev)
{
	return 0;
}

static void rk_ftl_release(struct mtd_blktrans_dev *dev)
{
}

/*
 * add_mtd: called by the blktrans core for each mtd device. We only bind to
 * the raw NAND our FTL understands. Build the L2P here (before exposing the
 * block device) so the first I/O already has a complete map.
 */
static void rk_ftl_add_mtd(struct mtd_blktrans_ops *tr, struct mtd_info *mtd)
{
	struct rk_ftl *ftl;

	/* Only whole raw-NAND MTDs with our page geometry. */
	if (mtd->type != MTD_NANDFLASH && mtd->type != MTD_MLCNANDFLASH)
		return;
	if (mtd->writesize != RK_FTL_PAGE_SIZE)
		return;

	ftl = kzalloc(sizeof(*ftl), GFP_KERNEL);
	if (!ftl)
		return;

	ftl->mtd = mtd;
	ftl->page_size = mtd->writesize;
	ftl->pages_per_block = mtd->erasesize / mtd->writesize;
	ftl->total_blocks = mtd_div_by_eb(mtd->size, mtd);
	ftl->total_pages = mtd_div_by_ws(mtd->size, mtd);
	ftl->page_buf_lpn = ~0u;

	ftl->oob_buf = kmalloc(mtd->oobsize, GFP_KERNEL);
	ftl->page_buf = kmalloc(ftl->page_size, GFP_KERNEL);
	if (!ftl->oob_buf || !ftl->page_buf)
		goto err_free;

	if (rk_ftl_build_map(ftl))
		goto err_free;

	/* Wire up the read-only block device. */
	ftl->mbd.mtd = mtd;
	ftl->mbd.devnum = mtd->index;
	ftl->mbd.tr = tr;
	ftl->mbd.readonly = 1;			/* Phase B: RO */
	/* size is in tr->blksize (512 B) units == capacity * spp sectors. */
	ftl->mbd.size = (unsigned long)ftl->capacity * RK_FTL_SPP;

	if (add_mtd_blktrans_dev(&ftl->mbd))
		goto err_free;			/* core frees nothing on fail */
	return;

err_free:
	vfree(ftl->l2p);
	kfree(ftl->region_to_ppa);
	kfree(ftl->page_buf);
	kfree(ftl->oob_buf);
	kfree(ftl);
}

static void rk_ftl_remove_dev(struct mtd_blktrans_dev *dev)
{
	struct rk_ftl *ftl = mbd_to_ftl(dev);

	del_mtd_blktrans_dev(dev);
	vfree(ftl->l2p);
	kfree(ftl->region_to_ppa);
	kfree(ftl->page_buf);
	kfree(ftl->oob_buf);
	/* dev is embedded in ftl; free after del_mtd_blktrans_dev drops refs. */
	kfree(ftl);
}

static struct mtd_blktrans_ops rk_ftl_tr = {
	.name		= RK_FTL_NAME,
	.major		= 0,			/* dynamic major */
	.part_bits	= 3,			/* allow partitions on the logical dev */
	.blksize	= RK_FTL_SECT_SIZE,	/* 512 */
	.readsect	= rk_ftl_readsect,
	/* no .writesect -> block device is read-only */
	.open		= rk_ftl_open,
	.release	= rk_ftl_release,
	.add_mtd	= rk_ftl_add_mtd,
	.remove_dev	= rk_ftl_remove_dev,
	.owner		= THIS_MODULE,
};

module_mtd_blktrans(rk_ftl_tr);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("s3plus rknand RE project");
MODULE_DESCRIPTION("Rockchip rknand read-only FTL (Phase B) - logical rootfs as a block device");
