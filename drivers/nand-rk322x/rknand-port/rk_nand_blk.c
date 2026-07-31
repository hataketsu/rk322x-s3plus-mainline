/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define pr_fmt(fmt) "rk_nand: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blkpg.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/semaphore.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/version.h>
/*
 * 6.6 forward-port: mainline has no in-tree Rockchip vendor-storage core
 * (<linux/soc/rockchip/rk_vendor_storage.h> and rk_vendor_register() are
 * out-of-tree symbols). The self-contained "vendor_storage" misc device is
 * still registered via rk_ftl_vendor_storage_init(); only the callback
 * registration into the (absent) vendor core is dropped.
 */

#include "rk_nand_blk.h"
#include "rk_ftl_api.h"

#define PART_READONLY 0x85
#define PART_WRITEONLY 0x86
#define PART_NO_ACCESS 0x87

/*
 * 6.6 forward-port: the closed FTL blob (rk_zftl_arm32.S, zftl_debug_proc_open)
 * emits a `bl PDE_DATA`, but mainline replaced PDE_DATA() with the static-inline
 * pde_data(). Provide a real, module-local PDE_DATA symbol so the blob's call
 * resolves at link/load time. Do not make static (must be visible to the blob).
 */
void *PDE_DATA(const struct inode *inode)
{
	return pde_data(inode);
}

static unsigned long totle_read_data;
static unsigned long totle_write_data;
static unsigned long totle_read_count;
static unsigned long totle_write_count;
static int rk_nand_dev_initialised;
static unsigned long rk_ftl_gc_do;
static DECLARE_WAIT_QUEUE_HEAD(rknand_thread_wait);
static unsigned long rk_ftl_gc_jiffies;

/*
 * Blank-NAND provisioning runs in its own kthread so module_init returns
 * immediately (a wedge in the FTL/NFC write path must not lock out the box).
 * rknand_provision_done is set once the thread has finished its work and is
 * idling; the exit path uses it to decide how to stop the thread.
 */
static struct task_struct *rknand_provision_task;
static int rknand_provision_done;
/*
 * Anything that can re-enter the (single-threaded, non-reentrant) FTL blob must
 * NOT exist while the blank-device format runs, or it wedges in D-state and
 * cascades to hang udev/logind/sshd. The GC thread, /proc/rknand and the
 * storage misc devices are therefore only brought up at the END of the provision
 * kthread, after nand_add_dev(). These flags gate a matching, guarded teardown.
 */
static int rknand_gc_started;
static int rknand_procfs_created;

static char *mtd_read_temp_buffer;
#define MTD_RW_SECTORS (512)

#define DISABLE_WRITE _IO('V', 0)
#define ENABLE_WRITE _IO('V', 1)
#define DISABLE_READ _IO('V', 2)
#define ENABLE_READ _IO('V', 3)
static int rknand_proc_show(struct seq_file *m, void *v)
{
	m->count = rknand_proc_ftlread(m->buf);
	seq_printf(m, "Totle Read %ld KB\n", totle_read_data >> 1);
	seq_printf(m, "Totle Write %ld KB\n", totle_write_data >> 1);
	seq_printf(m, "totle_write_count %ld\n", totle_write_count);
	seq_printf(m, "totle_read_count %ld\n", totle_read_count);
	return 0;
}

static int rknand_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, rknand_proc_show, pde_data(inode));
}

static const struct proc_ops rknand_proc_fops = {
	.proc_open	= rknand_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int rknand_create_procfs(void)
{
	struct proc_dir_entry *ent;

	ent = proc_create_data("rknand", 0444, NULL, &rknand_proc_fops,
			       (void *)0);
	if (!ent)
		return -1;

	return 0;
}

static struct mutex g_rk_nand_ops_mutex;

static void rknand_device_lock_init(void)
{
	mutex_init(&g_rk_nand_ops_mutex);
}

void rknand_device_lock(void)
{
	mutex_lock(&g_rk_nand_ops_mutex);
}

int rknand_device_trylock(void)
{
	return mutex_trylock(&g_rk_nand_ops_mutex);
}

void rknand_device_unlock(void)
{
	mutex_unlock(&g_rk_nand_ops_mutex);
}

static int nand_dev_transfer(struct nand_blk_dev *dev,
			     unsigned long start,
			     unsigned long nsector,
			     char *buf,
			     int cmd)
{
	int ret;

	if (dev->disable_access ||
	    ((cmd == WRITE) && dev->readonly) ||
	    ((cmd == READ) && dev->writeonly)) {
		return BLK_STS_IOERR;
	}

	start += dev->off_size;

	switch (cmd) {
	case READ:
		totle_read_data += nsector;
		totle_read_count++;
		ret = FtlRead(0, start, nsector, buf);
		if (ret)
			ret = BLK_STS_IOERR;
		break;

	case WRITE:
		totle_write_data += nsector;
		totle_write_count++;
		ret = FtlWrite(0, start, nsector, buf);
		if (ret)
			ret = BLK_STS_IOERR;
		break;

	default:
		ret = BLK_STS_IOERR;
		break;
	}

	return ret;
}

static int req_check_buffer_align(struct request *req, char **pbuf)
{
	int nr_vec = 0;
	struct bio_vec bv;
	struct req_iterator iter;
	char *buffer;
	void *firstbuf = 0;
	char *nextbuffer = 0;

	rq_for_each_segment(bv, req, iter) {
		/* high mem return 0 and using kernel buffer */
		if (PageHighMem(bv.bv_page))
			return 0;

		buffer = page_address(bv.bv_page) + bv.bv_offset;
		if (!buffer)
			return 0;
		if (!firstbuf)
			firstbuf = buffer;
		nr_vec++;
		if (nextbuffer && nextbuffer != buffer)
			return 0;
		nextbuffer = buffer + bv.bv_len;
	}
	*pbuf = firstbuf;
	return 1;
}

static blk_status_t do_blktrans_all_request(struct nand_blk_dev *dev,
					    struct request *req)
{
	unsigned long block, nsect;
	char *buf = NULL, *page_buf;
	struct req_iterator rq_iter;
	struct bio_vec bvec;
	int ret = BLK_STS_IOERR;
	unsigned long totle_nsect;

	block = blk_rq_pos(req);
	nsect = blk_rq_cur_bytes(req) >> 9;
	totle_nsect = (req->__data_len) >> 9;

	if (blk_rq_pos(req) + blk_rq_cur_sectors(req) > get_capacity(req->q->disk))
		return BLK_STS_IOERR;

	switch (req_op(req)) {
	case REQ_OP_DISCARD:
		if (FtlDiscard(block, nsect))
			return BLK_STS_IOERR;
		return BLK_STS_OK;
	case REQ_OP_READ:
		buf = mtd_read_temp_buffer;
		req_check_buffer_align(req, &buf);
		ret = nand_dev_transfer(dev, block, totle_nsect, buf, REQ_OP_READ);
		if (buf == mtd_read_temp_buffer) {
			char *p = buf;

			rq_for_each_segment(bvec, req, rq_iter) {
				page_buf = kmap_atomic(bvec.bv_page);

				memcpy(page_buf + bvec.bv_offset, p, bvec.bv_len);
				p += bvec.bv_len;
				kunmap_atomic(page_buf);
			}
		}

		if (ret)
			return BLK_STS_IOERR;
		else
			return BLK_STS_OK;
	case REQ_OP_WRITE:
		buf = mtd_read_temp_buffer;
		req_check_buffer_align(req, &buf);

		if (buf == mtd_read_temp_buffer) {
			char *p = buf;

			rq_for_each_segment(bvec, req, rq_iter) {
				page_buf = kmap_atomic(bvec.bv_page);
				memcpy(p, page_buf + bvec.bv_offset, bvec.bv_len);
				p += bvec.bv_len;
				kunmap_atomic(page_buf);
			}
		}

		ret = nand_dev_transfer(dev, block, totle_nsect, buf, REQ_OP_WRITE);

		if (ret)
			return BLK_STS_IOERR;
		else
			return BLK_STS_OK;

	default:
		return BLK_STS_IOERR;
	}
}

static struct request *rk_nand_next_request(struct nand_blk_dev *dev)
{
	struct nand_blk_ops *nand_ops = dev->nand_ops;
	struct request *rq;

	rq = list_first_entry_or_null(&nand_ops->rq_list, struct request, queuelist);
	if (rq) {
		list_del_init(&rq->queuelist);
		blk_mq_start_request(rq);
		return rq;
	}

	return NULL;
}

static void rk_nand_blktrans_work(struct nand_blk_dev *dev)
	__releases(&dev->nand_ops->queue_lock)
	__acquires(&dev->nand_ops->queue_lock)
{
	struct request *req = NULL;

	while (1) {
		blk_status_t res;

		req = rk_nand_next_request(dev);
		if (!req)
			break;

		spin_unlock_irq(&dev->nand_ops->queue_lock);

		rknand_device_lock();
		res = do_blktrans_all_request(dev, req);
		rknand_device_unlock();

		if (!blk_update_request(req, res, req->__data_len)) {
			__blk_mq_end_request(req, res);
			req = NULL;
		}

		spin_lock_irq(&dev->nand_ops->queue_lock);
	}
}

static blk_status_t rk_nand_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct nand_blk_dev *dev;

	dev = hctx->queue->queuedata;
	if (!dev) {
		blk_mq_start_request(bd->rq);
		return BLK_STS_IOERR;
	}

	rk_ftl_gc_do = 0;
	spin_lock_irq(&dev->nand_ops->queue_lock);
	list_add_tail(&bd->rq->queuelist, &dev->nand_ops->rq_list);
	rk_nand_blktrans_work(dev);
	spin_unlock_irq(&dev->nand_ops->queue_lock);

	/* wake up gc thread */
	rk_ftl_gc_do = 1;
	wake_up(&dev->nand_ops->thread_wq);

	return BLK_STS_OK;
}

static const struct blk_mq_ops rk_nand_mq_ops = {
	.queue_rq	= rk_nand_queue_rq,
};

static int nand_gc_thread(void *arg)
{
	struct nand_blk_ops *nand_ops = arg;
	int ftl_gc_status = 0;
	int req_empty_times = 0;
	int gc_done_times = 0;

	rk_ftl_gc_jiffies = HZ / 10;
	rk_ftl_gc_do = 1;

	while (!nand_ops->quit) {
		DECLARE_WAITQUEUE(wait, current);

		add_wait_queue(&nand_ops->thread_wq, &wait);
		set_current_state(TASK_INTERRUPTIBLE);

		if (rk_ftl_gc_do) {
			 /* do garbage collect at idle state */
			if (rknand_device_trylock()) {
				ftl_gc_status = rk_ftl_garbage_collect(1, 0);
				rknand_device_unlock();
				rk_ftl_gc_jiffies = HZ / 50;
				if (ftl_gc_status == 0) {
					gc_done_times++;
					if (gc_done_times > 10)
						rk_ftl_gc_jiffies = 10 * HZ;
					else
						rk_ftl_gc_jiffies = 1 * HZ;
				} else {
					gc_done_times = 0;
				}
			} else {
				rk_ftl_gc_jiffies = 1 * HZ;
			}
			req_empty_times++;
			if (req_empty_times < 10)
				rk_ftl_gc_jiffies = HZ / 50;
			/* cache write back after 100ms */
			if (req_empty_times >= 5 && req_empty_times < 7) {
				rknand_device_lock();
				rk_ftl_cache_write_back();
				rknand_device_unlock();
			}
		} else {
			req_empty_times = 0;
			rk_ftl_gc_jiffies = 1 * HZ;
		}
		wait_event_timeout(nand_ops->thread_wq, nand_ops->quit,
				   rk_ftl_gc_jiffies);
		remove_wait_queue(&nand_ops->thread_wq, &wait);
		continue;
	}
	pr_info("nand gc quited\n");
	nand_ops->nand_th_quited = 1;
	kthread_complete_and_exit(&nand_ops->thread_exit, 0);
	return 0;
}

static int rknand_open(struct gendisk *disk, blk_mode_t mode)
{
	return 0;
}

static void rknand_release(struct gendisk *disk)
{
};

static int rknand_ioctl(struct block_device *bdev, blk_mode_t mode,
			unsigned int cmd,
			unsigned long arg)
{
	struct nand_blk_dev *dev = bdev->bd_disk->private_data;

	switch (cmd) {
	case ENABLE_WRITE:
		dev->disable_access = 0;
		dev->readonly = 0;
		set_disk_ro(dev->blkcore_priv, 0);
		return 0;

	case DISABLE_WRITE:
		dev->readonly = 1;
		set_disk_ro(dev->blkcore_priv, 1);
		return 0;

	case ENABLE_READ:
		dev->disable_access = 0;
		dev->writeonly = 0;
		return 0;

	case DISABLE_READ:
		dev->writeonly = 1;
		return 0;
	default:
		return -ENOTTY;
	}
}

const struct block_device_operations nand_blktrans_ops = {
	.owner = THIS_MODULE,
	.open = rknand_open,
	.release = rknand_release,
	.ioctl = rknand_ioctl,
};

static struct nand_blk_ops mytr = {
	.name =  "rknand",
	/*
	 * Dynamic major. The vendor hard-coded 31, but that is the standard
	 * mtdblock major on mainline and collides (register_blkdev -EBUSY on a
	 * kernel that has mtdblock, and blocks reload). 0 => let register_blkdev
	 * allocate a free major; we capture the returned value below.
	 */
	.major = 0,
	.minorbits = 0,
	.owner = THIS_MODULE,
};

static int nand_add_dev(struct nand_blk_ops *nand_ops, struct nand_part *part)
{
	struct nand_blk_dev *dev;
	struct gendisk *gd;
	int ret;

	pr_info("rknand: nand_add_dev: part size=%lu sectors off=%lu type=%u\n",
		part->size, part->offset, part->type);
	if (part->size == 0) {
		pr_err("rknand: capacity is 0 -> NOT creating /dev/%s0 (FTL not ready?)\n",
		       nand_ops->name);
		return -1;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/*
	 * 6.6: alloc_disk()/blk_mq_init_sq_queue() are gone. blk_mq_alloc_disk()
	 * allocates the request_queue (from the already-initialised tag_set) and
	 * the gendisk together, and sets queue->queuedata = dev for us.
	 */
	gd = blk_mq_alloc_disk(nand_ops->tag_set, dev);
	if (IS_ERR(gd)) {
		ret = PTR_ERR(gd);
		kfree(dev);
		return ret;
	}
	nand_ops->rq = gd->queue;
	dev->nand_ops = nand_ops;
	dev->size = part->size;
	dev->off_size = part->offset;
	dev->devnum = nand_ops->last_dev_index;
	list_add_tail(&dev->list, &nand_ops->devs);
	nand_ops->last_dev_index++;

	blk_queue_max_hw_sectors(gd->queue, MTD_RW_SECTORS);
	blk_queue_max_segments(gd->queue, MTD_RW_SECTORS);

	/*
	 * 6.6: QUEUE_FLAG_DISCARD is gone; a non-zero max_discard_sectors is what
	 * advertises discard support now.
	 */
	blk_queue_max_discard_sectors(gd->queue, UINT_MAX >> 9);
	/* discard_granularity config to one nand page size 32KB*/
	gd->queue->limits.discard_granularity = 64 << 9;

	gd->major = nand_ops->major;
	gd->first_minor = (dev->devnum) << nand_ops->minorbits;

	gd->fops = &nand_blktrans_ops;

	/* 6.6: GENHD_FL_EXT_DEVT removed; default (0) keeps partition scanning */
	gd->flags = 0;
	gd->minors = 255;
	snprintf(gd->disk_name,
		 sizeof(gd->disk_name),
		 "%s%d",
		 nand_ops->name,
		 dev->devnum);

	set_capacity(gd, dev->size);

	gd->private_data = dev;
	dev->blkcore_priv = gd;

	if (part->type == PART_NO_ACCESS)
		dev->disable_access = 1;

	if (part->type == PART_READONLY)
		dev->readonly = 1;

	if (part->type == PART_WRITEONLY)
		dev->writeonly = 1;

	if (dev->readonly)
		set_disk_ro(gd, 1);

	/* 6.6: device_add_disk() now returns int and must be checked */
	ret = device_add_disk(g_nand_device, gd, NULL);
	pr_info("rknand: device_add_disk(%s) returned %d, capacity=%llu sectors\n",
		gd->disk_name, ret, (unsigned long long)get_capacity(gd));
	if (ret) {
		list_del(&dev->list);
		put_disk(gd);
		kfree(dev);
		return ret;
	}

	return 0;
}

static int nand_remove_dev(struct nand_blk_dev *dev)
{
	struct gendisk *gd;

	gd = dev->blkcore_priv;
	list_del(&dev->list);
	/*
	 * 6.6: the queue is owned by the gendisk (blk_mq_alloc_disk); del_gendisk()
	 * + put_disk() tears it down. Do not clear gd->queue by hand.
	 */
	del_gendisk(gd);
	put_disk(gd);
	kfree(dev);

	return 0;
}

/*
 * Blank-NAND provisioning + block-device creation, run in a kthread so a wedge
 * in the FTL/NFC write path (an NFC xfer-done wait that never completes during
 * FtlLowFormat) cannot block insmod and lock out the box. Verbose >>>/<<< logs
 * bracket every blob sub-call so the persistent kmsg stream pinpoints exactly
 * which call wedges. kthread_should_stop() is polled between steps so rmmod can
 * bail cleanly when the thread is *between* blob calls.
 */
static int rknand_provision_thread(void *arg)
{
	struct nand_blk_ops *nand_ops = arg;
	struct nand_part part;
	int cap, attempt, ret;

	memset(&part, 0, sizeof(part));

	cap = rk_ftl_get_capacity();
	pr_info("rknand: rk_ftl_get_capacity() = %d sectors (%d MB) [initial]\n",
		cap, cap >> 11);

	/*
	 * On a factory-blank/erased NAND the BBT and sys-block loads fail, so the
	 * FTL never set its internal DeviceCapacity -> capacity 0. Provision via
	 * the vendor's own reinit+auto-lowformat entry FtlReInitForSDUpdata()
	 * (FlashLoadFactorBbt/FlashMakeFactorBbt + FtlConstantsInit +
	 * retry-wrapped FtlLowFormat + FtlSysBlkInit in the correct order). A bare
	 * FtlLowFormat() would oops in FtlMakeBbt->FtlLoadFactoryBbt on a chip with
	 * no factory BBT. Fall back to FlashMakeFactorBbt()+FtlLowFormat().
	 */
	if (cap == 0 && !kthread_should_stop()) {
		int fmt, cap2;

		pr_info("rknand: capacity 0, provisioning blank NAND...\n");

		pr_info("rknand: >>> FtlReInitForSDUpdata()\n");
		fmt = FtlReInitForSDUpdata();
		pr_info("rknand: <<< FtlReInitForSDUpdata()=%d\n", fmt);

		pr_info("rknand: >>> rk_ftl_get_capacity() [post-reinit]\n");
		cap2 = rk_ftl_get_capacity();
		pr_info("rknand: <<< rk_ftl_get_capacity()=%d sectors (%d MB)\n",
			cap2, cap2 >> 11);

		if (cap2 == 0 && !kthread_should_stop()) {
			int lf, lb;

			pr_info("rknand: reinit yielded 0, trying FlashMakeFactorBbt()+FtlLowFormat()...\n");

			pr_info("rknand: >>> FlashLoadFactorBbt()\n");
			lb = FlashLoadFactorBbt();
			pr_info("rknand: <<< FlashLoadFactorBbt()=%d\n", lb);

			if (lb != 0 && !kthread_should_stop()) {
				pr_info("rknand: >>> FlashMakeFactorBbt()\n");
				lb = FlashMakeFactorBbt();
				pr_info("rknand: <<< FlashMakeFactorBbt()=%d\n", lb);
			}

			if (!kthread_should_stop()) {
				pr_info("rknand: >>> FtlLowFormat()\n");
				lf = FtlLowFormat();
				pr_info("rknand: <<< FtlLowFormat()=%d\n", lf);

				pr_info("rknand: >>> rk_ftl_get_capacity() [post-lowformat]\n");
				cap2 = rk_ftl_get_capacity();
				pr_info("rknand: <<< rk_ftl_get_capacity()=%d sectors (%d MB)\n",
					cap2, cap2 >> 11);
			}
		}
		cap = cap2;
	}

	/* bounded fallback retry in case capacity settles slightly later */
	for (attempt = 1; cap == 0 && attempt <= 10 && !kthread_should_stop();
	     attempt++) {
		msleep(100);
		cap = rk_ftl_get_capacity();
		pr_info("rknand: capacity retry %d -> %d sectors (%d MB)\n",
			attempt, cap, cap >> 11);
	}

	part.offset = 0;
	part.type = 0;
	part.name[0] = 0;
	part.size = cap;

	/*
	 * Guard: if provisioning left capacity 0, do NOT create the disk or bring
	 * up anything that touches the FTL. Leave a clean, still-responsive box
	 * that can simply be rmmod'd.
	 */
	if (cap == 0) {
		pr_err("rknand: capacity still 0 after provisioning; leaving FTL idle (no disk, no GC, no procfs)\n");
		goto done;
	}

	if (kthread_should_stop())
		goto done;

	ret = nand_add_dev(nand_ops, &part);
	pr_info("rknand: nand_add_dev() returned %d (size=%lu sectors)\n",
		ret, part.size);
	if (ret) {
		pr_err("rknand: nand_add_dev failed (%d); not starting GC/procfs\n", ret);
		goto done;
	}

	/*
	 * Provisioning is complete and /dev/rknand0 exists. Only NOW is it safe to
	 * bring up the other FTL users: the GC thread, /proc/rknand and the storage
	 * misc devices. Order: GC last-ish, but each guarded/tracked for teardown.
	 */
	pr_info("rknand: >>> starting GC thread + procfs + storage\n");

	if (rknand_create_procfs() == 0)
		rknand_procfs_created = 1;

	rk_ftl_storage_sys_init();

	ret = rk_ftl_vendor_storage_init();
	if (!ret) {
		/*
		 * 6.6: no in-tree rk_vendor_storage core to register callbacks into.
		 * The self-contained "vendor_storage" misc device is still exposed
		 * via rknand_vendor_storage_init().
		 */
		rknand_vendor_storage_init();
		pr_info("rknand vendor storage init ok !\n");
	} else {
		pr_info("rknand vendor storage init failed !\n");
	}

	nand_ops->quit = 0;
	nand_ops->nand_th_quited = 0;
	nand_ops->gc_task = kthread_run(nand_gc_thread, (void *)nand_ops,
					"rknand_gc");
	if (IS_ERR(nand_ops->gc_task)) {
		pr_err("rknand: failed to start GC thread: %ld\n",
		       PTR_ERR(nand_ops->gc_task));
		nand_ops->gc_task = NULL;
	} else {
		rknand_gc_started = 1;
	}
	pr_info("rknand: <<< GC/procfs/storage up\n");

done:
	/*
	 * Work is done. Stay alive and joinable until module exit calls
	 * kthread_stop(); this avoids the task_struct being reaped and makes the
	 * exit-path join well-defined.
	 */
	rknand_provision_done = 1;
	pr_info("rknand: provision thread finished, idling until module exit\n");
	while (!kthread_should_stop())
		msleep(200);
	return 0;
}

static int nand_blk_register(struct nand_blk_ops *nand_ops)
{
	int ret;

	/*
	 * s3plus: schedule/wait_event mode (1), as the vendor intends for runtime.
	 * Root cause of the earlier "IRQ never fires" scare: the completion IRQ
	 * that matters here is the NFC *flash-ready* IRQ (int_en@0x16C bit1), and
	 * the linked FTL blob (rk_zftl_arm32.o) arms it itself inside
	 * nandc_iqr_wait_flash_ready() right before each program/erase busy-wait
	 * (then disables it on completion to avoid a level-triggered storm while
	 * the flash idles ready). The DMA/xfer-completion IRQ (bit0) is never used
	 * -- the whole data path (nandc_xfer_done) and the entire FTLv5 core
	 * (FtlLowFormat / FtlReInitForSDUpdata provisioning) poll via
	 * usleep_range/ndelay, so GIC-47 staying 0 during poll-mode detection was
	 * expected, not a bug. wait_for_nand_flash_ready() now degrades to a short
	 * (~4ms) timeout re-poll instead of the old 20ms one-shot timer hack, so
	 * schedule mode cannot crawl/wedge even if a ready-IRQ wake is ever missed.
	 */
	rk_nand_schedule_enable_config(1);
	nand_ops->quit = 0;
	nand_ops->nand_th_quited = 0;

	/*
	 * With major == 0 register_blkdev() returns the allocated major (>0) on
	 * success; with a fixed major it returns 0. Negative is an error. Capture
	 * the dynamic major so gd->major and the unregister path use the real one.
	 */
	ret = register_blkdev(nand_ops->major, nand_ops->name);
	if (ret < 0) {
		pr_err("rknand: register_blkdev failed: %d\n", ret);
		return ret;
	}
	if (nand_ops->major == 0)
		nand_ops->major = ret;
	pr_info("rknand: registered block major %d\n", nand_ops->major);

	mtd_read_temp_buffer = kmalloc(MTD_RW_SECTORS * 512, GFP_KERNEL | GFP_DMA);
	if (!mtd_read_temp_buffer) {
		ret = -ENOMEM;
		goto mtd_buffer_error;
	}

	init_completion(&nand_ops->thread_exit);
	init_waitqueue_head(&nand_ops->thread_wq);
	rknand_device_lock_init();

	/* Create the request queue */
	spin_lock_init(&nand_ops->queue_lock);
	INIT_LIST_HEAD(&nand_ops->rq_list);

	nand_ops->tag_set = kzalloc(sizeof(*nand_ops->tag_set), GFP_KERNEL);
	if (!nand_ops->tag_set) {
		ret = -ENOMEM;
		goto tag_set_error;
	}

	/*
	 * 6.6: blk_mq_init_sq_queue() is gone. Initialise the tag_set explicitly;
	 * the request_queue is created together with the gendisk by
	 * blk_mq_alloc_disk() in nand_add_dev(). Queue limits are set there too.
	 */
	nand_ops->tag_set->ops = &rk_nand_mq_ops;
	nand_ops->tag_set->nr_hw_queues = 1;
	nand_ops->tag_set->nr_maps = 1;
	nand_ops->tag_set->queue_depth = 1;
	nand_ops->tag_set->numa_node = NUMA_NO_NODE;
	nand_ops->tag_set->flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING;
	ret = blk_mq_alloc_tag_set(nand_ops->tag_set);
	if (ret)
		goto rq_init_error;

	INIT_LIST_HEAD(&nand_ops->devs);
	nand_ops->last_dev_index = 0;

	/*
	 * Everything that can touch the FTL blob (the GC thread, /proc/rknand and
	 * the storage misc devices) is deferred to the END of the provision
	 * kthread, after nand_add_dev(). Starting any of them here would let a
	 * second context re-enter the non-reentrant FTL during the multi-second
	 * blank-device format -> D-state hang cascading to udev/logind/sshd.
	 * module_init only does FTL-free setup and returns as soon as the provision
	 * kthread is started, so the box stays fully responsive during the format.
	 */
	rknand_provision_done = 0;
	rknand_provision_task = kthread_run(rknand_provision_thread,
					    (void *)nand_ops, "rknand_provision");
	if (IS_ERR(rknand_provision_task)) {
		ret = PTR_ERR(rknand_provision_task);
		rknand_provision_task = NULL;
		pr_err("rknand: failed to start provision thread: %d\n", ret);
		/* not fatal to module load; the block device just won't appear */
	}

	return 0;

rq_init_error:
	kfree(nand_ops->tag_set);
tag_set_error:
	kfree(mtd_read_temp_buffer);
	mtd_read_temp_buffer = NULL;
mtd_buffer_error:
	unregister_blkdev(nand_ops->major, nand_ops->name);

	return ret;
}

static void nand_blk_unregister(struct nand_blk_ops *nand_ops)
{
	struct list_head *this, *next;

	if (!rk_nand_dev_initialised)
		return;

	/*
	 * Stop the provision kthread first (it may still be about to create the
	 * disk). kthread_stop() requests stop and joins. If the thread is wedged
	 * inside a blocking blob/NFC call it cannot observe kthread_should_stop()
	 * and this will block rmmod -- but the box is already wedged in that case;
	 * we never free the module out from under a running thread (no crash).
	 */
	if (rknand_provision_task) {
		if (!rknand_provision_done)
			pr_warn("rknand: provision thread still running at rmmod; joining (may block if wedged in FTL)\n");
		kthread_stop(rknand_provision_task);
		rknand_provision_task = NULL;
	}

	/*
	 * Stop the GC thread only if it was actually started (it now comes up at
	 * the end of provisioning, not in register). Otherwise thread_exit is never
	 * completed and wait_for_completion() would hang.
	 */
	if (rknand_gc_started) {
		nand_ops->quit = 1;
		wake_up(&nand_ops->thread_wq);
		wait_for_completion(&nand_ops->thread_exit);
		nand_ops->gc_task = NULL;
		rknand_gc_started = 0;
	}

	list_for_each_safe(this, next, &nand_ops->devs) {
		struct nand_blk_dev *dev
			= list_entry(this, struct nand_blk_dev, list);

		nand_remove_dev(dev);
	}
	/*
	 * 6.6: blk_cleanup_queue() is gone. Each disk's queue is destroyed by
	 * its put_disk() in nand_remove_dev(); free the shared tag_set here.
	 */
	if (nand_ops->tag_set) {
		blk_mq_free_tag_set(nand_ops->tag_set);
		kfree(nand_ops->tag_set);
		nand_ops->tag_set = NULL;
	}
	/* remove /proc/rknand only if we created it, so reload can re-create it */
	if (rknand_procfs_created) {
		remove_proc_entry("rknand", NULL);
		rknand_procfs_created = 0;
	}
	if (mtd_read_temp_buffer) {
		kfree(mtd_read_temp_buffer);
		mtd_read_temp_buffer = NULL;
	}
	unregister_blkdev(nand_ops->major, nand_ops->name);
}

void rknand_dev_flush(void)
{
	if (!rk_nand_dev_initialised)
		return;
	rknand_device_lock();
	rk_ftl_cache_write_back();
	rknand_device_unlock();
	pr_info("Nand flash flush ok!\n");
}

int __init rknand_dev_init(void)
{
	int ret;
	void __iomem *nandc0;
	void __iomem *nandc1;

	rknand_get_reg_addr((unsigned long *)&nandc0, (unsigned long *)&nandc1);
	if (!nandc0)
		return -1;

	ret = rk_ftl_init();
	if (ret) {
		pr_err("rk_ftl_init fail\n");
		return -1;
	}

	ret = nand_blk_register(&mytr);
	if (ret) {
		pr_err("nand_blk_register fail\n");
		return -1;
	}

	rk_nand_dev_initialised = 1;
	return ret;
}

int rknand_dev_exit(void)
{
	if (!rk_nand_dev_initialised)
		return -1;
	rk_nand_dev_initialised = 0;
	if (rknand_device_trylock()) {
		rk_ftl_cache_write_back();
		rknand_device_unlock();
	}
	nand_blk_unregister(&mytr);
	rk_ftl_de_init();
	pr_info("nand_blk_dev_exit:OK\n");
	return 0;
}

void rknand_dev_suspend(void)
{
	if (!rk_nand_dev_initialised)
		return;
	pr_info("rk_nand_suspend\n");
	rk_nand_schedule_enable_config(0);
	rknand_device_lock();
	rk_nand_suspend();
}

void rknand_dev_resume(void)
{
	if (!rk_nand_dev_initialised)
		return;
	pr_info("rk_nand_resume\n");
	rk_nand_resume();
	rknand_device_unlock();
	rk_nand_schedule_enable_config(1);
}

void rknand_dev_shutdown(void)
{
	pr_info("rknand_shutdown...\n");
	if (!rk_nand_dev_initialised)
		return;
	/* Only join the GC thread if it was actually started (post-provision). */
	if (rknand_gc_started && mytr.quit == 0) {
		mytr.quit = 1;
		wake_up(&mytr.thread_wq);
		wait_for_completion(&mytr.thread_exit);
		mytr.gc_task = NULL;
		rknand_gc_started = 0;
		rk_ftl_de_init();
	}
	pr_info("rknand_shutdown:OK\n");
}
