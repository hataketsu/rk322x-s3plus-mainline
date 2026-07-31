/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/cacheflush.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#endif
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>

#include "rk_nand_blk.h"
#include "rk_ftl_api.h"
#include "rk_nand_base.h"

#define RKNAND_VERSION_AND_DATE  "rknandbase v1.2 2021-01-07"

static struct rk_nandc_info g_nandc_info[2];
struct device *g_nand_device;
static char nand_idb_data[2048];
static int rk_nand_wait_busy_schedule;
static int rk_nand_suspend_state;
static int rk_nand_shutdown_state;
/*1:flash 2:emmc 4:sdcard0 8:sdcard1*/
static int rknand_boot_media = 2;
static DECLARE_WAIT_QUEUE_HEAD(rk29_nandc_wait);
static int nandc0_xfer_completed_flag;
static int nandc0_ready_completed_flag;
static int nandc1_xfer_completed_flag;
static int nandc1_ready_completed_flag;

/*
 * s3plus: bounded fallback for the schedule/wait_event path. The linked FTL
 * blob (rk_zftl_arm32.o) arms the NFC *flash-ready* IRQ (int_en@0x16C bit1)
 * itself, inside nandc_iqr_wait_flash_ready(), immediately before each
 * program/erase busy-wait, and disables it again in its completion handler
 * (a level-triggered ready IRQ must not stay armed while the flash idles
 * ready, or it storms -- which is exactly why the kernel must NOT blindly
 * write int_en). So wait_event() here is woken at IRQ speed *when* the ready
 * IRQ fires. To guarantee the box can never crawl/wedge if a wake is missed,
 * every wait falls back to a short timeout re-poll instead of the old 20ms
 * (HZ/50) one-shot timer hack. HZ=250 -> ~4ms worst-case fallback.
 */
#define RK_NAND_WAIT_FALLBACK_JIFFIES 1

void *ftl_malloc(int size)
{
	return kmalloc(size, GFP_KERNEL | GFP_DMA);
}

void ftl_free(void *buf)
{
	kfree(buf);
}

int rknand_get_clk_rate(int nandc_id)
{
	return g_nandc_info[nandc_id].clk_rate;
}
EXPORT_SYMBOL(rknand_get_clk_rate);

/*
 * s3plus: NAND writes were silently dropped until the flash WP# pin was muxed.
 *
 * FlashMakeFactorBbt programmed its test pattern, the chip reported ready and
 * status PASS, and the readback came back 0xFFFFFFFF ("prog read s/d error:
 * ... ffffffff"), so the FTL marked every block bad. Reading the media back
 * with the mainline rockchip-nand-controller showed the cells were still
 * erased: nothing had ever been programmed. The mainline driver could not
 * erase or write either (-EIO), which is what identified the cause as the
 * board rather than the FTL: the device-tree overlay that enables the NFC node
 * listed the flash-ale/bus8/cle/cs0/dqs/rdn/rdy/wrn pinctrl groups but not
 * flash-wp, so WP# was never muxed to the controller and stayed asserted.
 * Adding that group to pinctrl-0 makes both drivers program the chip normally.
 *
 * FMCTL bit8 is the controller's WP# output level (mainline calls it FMCTL_WP
 * and sets it once in rk_nfc_hw_init()). The blob's NandcInit sets it too, but
 * only while it owns the controller, so this keeps it asserted on every program
 * and says so once if it ever had to intervene.
 */
#define RKNAND_V6_FMCTL			0x00
#define RKNAND_V6_FMCTL_WP		BIT(8)

static void rknand_force_wp_deasserted(void)
{
	void __iomem *base = g_nandc_info[0].reg_base;
	static int logged;
	u32 fmctl;

	if (!base)
		return;

	fmctl = readl(base + RKNAND_V6_FMCTL);
	if (!(fmctl & RKNAND_V6_FMCTL_WP)) {
		writel(fmctl | RKNAND_V6_FMCTL_WP, base + RKNAND_V6_FMCTL);
		if (!logged) {
			logged = 1;
			pr_err("rknand: FMCTL=0x%08x at program time: WP# was asserted (bit8 clear) - forcing it high\n",
			       fmctl);
		}
	}
}

unsigned long rknand_dma_map_single(unsigned long ptr, int size, int dir)
{
	dma_addr_t addr;

	if (dir)
		rknand_force_wp_deasserted();

	addr = dma_map_single(g_nand_device, (void *)ptr, size
		, dir ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	if (dma_mapping_error(g_nand_device, addr))
		pr_err_once("rknand: dma_map_single FAILED (ptr=0x%lx size=%d dir=%d) - NFC would transfer garbage\n",
			    ptr, size, dir);
	return addr;
}
EXPORT_SYMBOL(rknand_dma_map_single);

void rknand_dma_unmap_single(unsigned long ptr, int size, int dir)
{
	dma_unmap_single(g_nand_device, (dma_addr_t)ptr, size
		, dir ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}
EXPORT_SYMBOL(rknand_dma_unmap_single);

int rknand_flash_cs_init(int id)
{
	return 0;
}
EXPORT_SYMBOL(rknand_flash_cs_init);

int rknand_get_reg_addr(unsigned long *p_nandc0, unsigned long *p_nandc1)
{
	*p_nandc0 = (unsigned long)g_nandc_info[0].reg_base;
	*p_nandc1 = (unsigned long)g_nandc_info[1].reg_base;
	return 0;
}
EXPORT_SYMBOL(rknand_get_reg_addr);

int rknand_get_boot_media(void)
{
	return rknand_boot_media;
}
EXPORT_SYMBOL(rknand_get_boot_media);

unsigned long rk_copy_from_user(void *to, const void __user *from,
				unsigned long n)
{
	return copy_from_user(to, from, n);
}

unsigned long rk_copy_to_user(void __user *to, const void *from,
			      unsigned long n)
{
	return copy_to_user(to, from, n);
}

static const struct file_operations rknand_sys_storage_fops = {
	.compat_ioctl = rknand_sys_storage_ioctl,
	.unlocked_ioctl = rknand_sys_storage_ioctl,
};

static struct miscdevice rknand_sys_storage_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "rknand_sys_storage",
	.fops  = &rknand_sys_storage_fops,
};

/*
 * Track misc-device registration so the module exit path can deregister them.
 * Without this, rmmod leaves the two miscdevices registered pointing at this
 * module's (now freed) static structs -> a reload/oops hazard.
 */
static int rknand_sys_storage_registered;
static int rknand_vender_storage_registered;

int rknand_sys_storage_init(void)
{
	int ret = misc_register(&rknand_sys_storage_dev);

	if (!ret)
		rknand_sys_storage_registered = 1;
	return ret;
}

static const struct file_operations rknand_vendor_storage_fops = {
	.compat_ioctl	= rk_ftl_vendor_storage_ioctl,
	.unlocked_ioctl = rk_ftl_vendor_storage_ioctl,
};

static struct miscdevice rknand_vender_storage_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "vendor_storage",
	.fops  = &rknand_vendor_storage_fops,
};

int rknand_vendor_storage_init(void)
{
	int ret = misc_register(&rknand_vender_storage_dev);

	if (!ret)
		rknand_vender_storage_registered = 1;
	return ret;
}

void rknand_misc_deinit(void)
{
	if (rknand_sys_storage_registered) {
		misc_deregister(&rknand_sys_storage_dev);
		rknand_sys_storage_registered = 0;
	}
	if (rknand_vender_storage_registered) {
		misc_deregister(&rknand_vender_storage_dev);
		rknand_vender_storage_registered = 0;
	}
}

int rk_nand_schedule_enable_config(int en)
{
	int tmp = rk_nand_wait_busy_schedule;

	rk_nand_wait_busy_schedule = en;
	return tmp;
}

static irqreturn_t rk_nandc_interrupt(int irq, void *dev_id)
{
	unsigned int irq_status = rk_nandc_get_irq_status(dev_id);

	if (irq_status & (1 << 0)) {
		rk_nandc_flash_xfer_completed(dev_id);
		if (dev_id == g_nandc_info[0].reg_base)
			nandc0_xfer_completed_flag = 1;
		else
			nandc1_xfer_completed_flag = 1;
	}

	if (irq_status & (1 << 1)) {
		rk_nandc_flash_ready(dev_id);
		if (dev_id == g_nandc_info[0].reg_base)
			nandc0_ready_completed_flag = 1;
		else
			nandc1_ready_completed_flag = 1;
	}

	wake_up(&rk29_nandc_wait);
	return IRQ_HANDLED;
}

void rk_nandc_xfer_irq_flag_init(void *nandc_reg)
{
	if (nandc_reg == g_nandc_info[0].reg_base)
		nandc0_xfer_completed_flag = 0;
	else
		nandc1_xfer_completed_flag = 0;
}

void rk_nandc_rb_irq_flag_init(void *nandc_reg)
{
	if (nandc_reg == g_nandc_info[0].reg_base)
		nandc0_ready_completed_flag = 0;
	else
		nandc1_ready_completed_flag = 0;
}

void wait_for_nandc_xfer_completed(void *nandc_reg)
{
	if (rk_nand_wait_busy_schedule) {
		if (nandc_reg == g_nandc_info[0].reg_base)
			while (!wait_event_timeout(rk29_nandc_wait,
						   nandc0_xfer_completed_flag,
						   RK_NAND_WAIT_FALLBACK_JIFFIES))
				;
		else
			while (!wait_event_timeout(rk29_nandc_wait,
						   nandc1_xfer_completed_flag,
						   RK_NAND_WAIT_FALLBACK_JIFFIES))
				;
	}
	if (nandc_reg == g_nandc_info[0].reg_base)
		nandc0_xfer_completed_flag = 0;
	else
		nandc1_xfer_completed_flag = 0;
}

void wait_for_nand_flash_ready(void *nandc_reg)
{
	if (rk_nand_wait_busy_schedule) {
		if (nandc_reg == g_nandc_info[0].reg_base)
			while (!wait_event_timeout(rk29_nandc_wait,
						   nandc0_ready_completed_flag,
						   RK_NAND_WAIT_FALLBACK_JIFFIES))
				;
		else
			while (!wait_event_timeout(rk29_nandc_wait,
						   nandc1_ready_completed_flag,
						   RK_NAND_WAIT_FALLBACK_JIFFIES))
				;
	}
	if (nandc_reg == g_nandc_info[0].reg_base)
		nandc0_ready_completed_flag = 0;
	else
		nandc1_ready_completed_flag = 0;
}

static int rk_nandc_irq_config(int id, int mode, void *pfun)
{
	int ret = 0;
	int irq = g_nandc_info[id].irq;

	if (mode)
		ret = request_irq(irq, pfun, 0, "nandc"
			, g_nandc_info[id].reg_base);
	else
		free_irq(irq,  NULL);
	return ret;
}

int rk_nandc_irq_init(void)
{
	int ret = 0;

	nandc0_ready_completed_flag = 0;
	nandc0_xfer_completed_flag = 0;
	ret = rk_nandc_irq_config(0, 1, rk_nandc_interrupt);

	if (!g_nandc_info[1].reg_base) {
		nandc1_ready_completed_flag = 0;
		nandc1_xfer_completed_flag = 0;
		rk_nandc_irq_config(1, 1, rk_nandc_interrupt);
	}
	return ret;
}

int rk_nandc_irq_deinit(void)
{
	rk_nandc_irq_config(0, 0, rk_nandc_interrupt);
	if (!g_nandc_info[1].reg_base)
		rk_nandc_irq_config(1, 0, rk_nandc_interrupt);
	return 0;
}

static int rknand_probe(struct platform_device *pdev)
{
	unsigned int id = 0;
	int irq;
	struct resource	*mem;
	void __iomem	*membase;

	g_nand_device = &pdev->dev;
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	membase = devm_ioremap_resource(&pdev->dev, mem);
	if (!membase) {
		dev_err(&pdev->dev, "no reg resource?\n");
		return -1;
	}

	#ifdef CONFIG_OF
	of_property_read_u32(pdev->dev.of_node, "nandc_id", &id);
	pdev->id = id;
	#endif

	if (id == 0) {
		memcpy(nand_idb_data, membase + 0x1000, 0x800);
		if (*(int *)(&nand_idb_data[0]) == 0x44535953) {
			rknand_boot_media = *(int *)(&nand_idb_data[8]);
			if (rknand_boot_media == 2) /*boot from emmc*/
				return -1;
		}
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource?\n");
		return irq;
	}

	g_nandc_info[id].id = id;
	g_nandc_info[id].irq = irq;
	g_nandc_info[id].reg_base = membase;

	g_nandc_info[id].hclk = devm_clk_get(&pdev->dev, "hclk_nandc");
	g_nandc_info[id].clk = devm_clk_get(&pdev->dev, "clk_nandc");
	g_nandc_info[id].gclk = devm_clk_get(&pdev->dev, "g_clk_nandc");

	if (unlikely(IS_ERR(g_nandc_info[id].hclk))) {
		dev_err(&pdev->dev, "rknand_probe get hclk error\n");
		return PTR_ERR(g_nandc_info[id].hclk);
	}

	if (!(IS_ERR(g_nandc_info[id].clk))) {
		clk_set_rate(g_nandc_info[id].clk, 150 * 1000 * 1000);
		g_nandc_info[id].clk_rate = clk_get_rate(g_nandc_info[id].clk);
		clk_prepare_enable(g_nandc_info[id].clk);
		dev_info(&pdev->dev,
			 "rknand_probe clk rate = %d\n",
			 g_nandc_info[id].clk_rate);
	}

	clk_prepare_enable(g_nandc_info[id].hclk);
	if (!(IS_ERR(g_nandc_info[id].gclk)))
		clk_prepare_enable(g_nandc_info[id].gclk);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	return dma_set_mask(g_nand_device, DMA_BIT_MASK(32));
}

static int rknand_suspend(struct platform_device *pdev, pm_message_t state)
{
	if (rk_nand_suspend_state == 0) {
		rk_nand_suspend_state = 1;
		rknand_dev_suspend();
	}
	return 0;
}

static int rknand_resume(struct platform_device *pdev)
{
	if (rk_nand_suspend_state == 1) {
		rk_nand_suspend_state = 0;
		rknand_dev_resume();
	}
	return 0;
}

static void rknand_shutdown(struct platform_device *pdev)
{
	if (rk_nand_shutdown_state == 0) {
		rk_nand_shutdown_state = 1;
		rknand_dev_shutdown();
	}
}

void rknand_dev_cache_flush(void)
{
	rknand_dev_flush();
}

static int rknand_pm_suspend(struct device *dev)
{
	if (rk_nand_suspend_state == 0) {
		rk_nand_suspend_state = 1;
		rknand_dev_suspend();
		pm_runtime_put(dev);
	}
	return 0;
}

static int rknand_pm_resume(struct device *dev)
{
	if (rk_nand_suspend_state == 1) {
		rk_nand_suspend_state = 0;
		pm_runtime_get_sync(dev);
		rknand_dev_resume();
	}
	return 0;
}

static const struct dev_pm_ops rknand_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rknand_pm_suspend, rknand_pm_resume)
};

#ifdef CONFIG_OF
static const struct of_device_id of_rk_nandc_match[] = {
	{.compatible = "rockchip,rk-nandc"},
	{.compatible = "rockchip,rk-nandc-v9"},
	{}
};
#endif

static struct platform_driver rknand_driver = {
	.probe		= rknand_probe,
	.suspend	= rknand_suspend,
	.resume		= rknand_resume,
	.shutdown	= rknand_shutdown,
	.driver		= {
		.name	= "rknand",
#ifdef CONFIG_OF
		.of_match_table	= of_rk_nandc_match,
#endif
		.pm = &rknand_dev_pm_ops,
	},
};

static void __exit rknand_driver_exit(void)
{
	rknand_dev_exit();
	rknand_misc_deinit();
	platform_driver_unregister(&rknand_driver);
}

static int __init rknand_driver_init(void)
{
	int ret = 0;

	pr_err("%s\n", RKNAND_VERSION_AND_DATE);
	ret = platform_driver_register(&rknand_driver);
	if (ret)
		return ret;
	ret = rknand_dev_init();
	if (ret) {
		/*
		 * Unwind on failure: leaving the platform driver registered while
		 * the module is torn down leaves the driver-core with dangling
		 * pointers into freed module text (the register-dump/oops seen on
		 * the failed reload path). Also drop any misc devices registered
		 * before the failure.
		 */
		pr_err("rknand: rknand_dev_init failed (%d), unwinding\n", ret);
		rknand_misc_deinit();
		platform_driver_unregister(&rknand_driver);
	}
	return ret;
}

module_init(rknand_driver_init);
module_exit(rknand_driver_exit);
MODULE_ALIAS("rknand");
MODULE_LICENSE("GPL v2");
