/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __RK_FTL_API_H
#define __RK_FTL_API_H

void rk_nandc_flash_xfer_completed(void *nandc_reg);
void rk_nandc_flash_ready(void *nandc_reg);
u32 rk_nandc_get_irq_status(void *nandc_reg);
int rknand_proc_ftlread(char *page);
int FtlRead(u8 lun, u32 index, u32 sectors, u8 *buf);
int FtlWrite(u8 lun, u32 index, u32 sectors, u8 *buf);
int rk_ftl_garbage_collect(u32 mode, u32 pages);
void rk_ftl_cache_write_back(void);
int FtlDiscard(u32 index, u32 sectors);
int rk_nand_schedule_enable_config(int en);
int rk_ftl_get_capacity(void);
/*
 * Provision a factory-blank/erased NAND: FtlLowFormat() writes the vendor SLC
 * metadata and sets the blob's internal DeviceCapacity. Needed when both the
 * BBT and sys-block loads fail (blank device), where rk_ftl_get_capacity()
 * would otherwise return 0. Exported by rk_ftlv5_arm32.S; returns 0 on success.
 */
int FtlLowFormat(void);
/*
 * Vendor blank-NAND provisioning entry points (all exported by rk_ftlv5_arm32.S).
 * FtlReInitForSDUpdata() does FlashLoadFactorBbt/FlashMakeFactorBbt +
 * FtlConstantsInit + retry-wrapped FtlLowFormat + FtlSysBlkInit in the correct
 * order. Calling bare FtlLowFormat() on a chip with no factory BBT oopses in
 * FtlMakeBbt->FtlLoadFactoryBbt (out-of-range block -> NULL region_bitmap), so
 * this reinit path (with a FlashMakeFactorBbt()+FtlLowFormat() fallback) is used.
 */
int FtlReInitForSDUpdata(void);
int FlashMakeFactorBbt(void);
int FlashLoadFactorBbt(void);
void rk_ftl_storage_sys_init(void);
int rk_ftl_init(void);
void rk_nand_de_init(void);
void rk_ftl_de_init(void);
void rk_nand_suspend(void);
void rk_nand_resume(void);
int rknand_get_reg_addr(unsigned long *p_nandc0, unsigned long *p_nandc1);
long rknand_sys_storage_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg);
long rk_ftl_vendor_storage_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
int rk_ftl_vendor_write(u32 id, void *pbuf, u32 size);
int rk_ftl_vendor_read(u32 id, void *pbuf, u32 size);
int rk_ftl_vendor_storage_init(void);
int rknand_vendor_storage_init(void);

#endif

