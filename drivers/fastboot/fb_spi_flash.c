// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Modified for SPI-NAND (MTD) support
 * Replaces standard SPI Flash backend with MTD backend in-place.
 */

#include <blk.h>
#include <config.h>
#include <dm.h>
#include <env.h>
#include <fastboot.h>
#include <image-sparse.h>
#include <log.h>             /* 替代 common.h 中的 printf/debug */
#include <malloc.h>
#include <string.h>          /* 必须包含，用于 strncpy, strcmp */
#include <vsprintf.h>        /* 用于 simple_strtoul 等 */
#include <linux/errno.h>     /* 用于 -ENODEV 等错误码 */
#include <linux/mtd/mtd.h>   /* Linux 风格 MTD 核心定义 */
#include <linux/types.h>     /* 基础数据类型 */
#include <mtd.h>             /* U-Boot 特有的 mtd_probe_devices */

/* 
 * ------------------------------------------------------------
 * 核心辅助函数：根据 Fastboot 传入的分区名获取 MTD 设备
 * ------------------------------------------------------------
 */
static int get_mtd_device_by_name(const char *name, struct mtd_info **mtd)
{
	int ret;

	/* 1. 确保 MTD 子系统已初始化并探测到所有设备 */
	mtd_probe_devices();

	/* 2. 尝试根据名称获取 MTD 分区 (例如 "u-boot", "kernel", "rootfs") */
	*mtd = get_mtd_device_nm(name);
	
	/* 3. 错误处理 */
	if (IS_ERR(*mtd)) {
		ret = PTR_ERR(*mtd);
		/* 如果没找到，尝试再次刷新探测（针对某些懒加载的驱动） */
		if (ret == -ENODEV) {
			mtd_probe_devices();
			*mtd = get_mtd_device_nm(name);
			if (IS_ERR(*mtd))
				return PTR_ERR(*mtd);
		} else {
			return ret;
		}
	}
	return 0;
}

/*
 * ------------------------------------------------------------
 * Sparse Image (稀疏镜像) 写入回调
 * ------------------------------------------------------------
 */
static lbaint_t fb_mtd_sparse_write(struct sparse_storage *info,
				    lbaint_t blk, lbaint_t blkcnt,
				    const void *buffer)
{
	struct mtd_info *mtd = (struct mtd_info *)info->priv;
	size_t len = blkcnt * info->blksz;
	u32 offset = blk * info->blksz; /* 这里的 offset 是相对于分区起点的 */
	size_t retlen;
	int ret;

	/* 这里的 offset 是相对分区的，MTD 驱动会自动加上分区的物理偏移 */
	ret = mtd_write(mtd, offset, len, &retlen, buffer);
	if (ret) {
		printf("MTD write error at offset 0x%x: %d\n", offset, ret);
		return 0;
	}

	return blkcnt;
}

static lbaint_t fb_mtd_sparse_reserve(struct sparse_storage *info,
				      lbaint_t blk, lbaint_t blkcnt)
{
	/* MTD 自动处理跳过坏块（如果底层驱动支持），这里直接返回成功 */
	return blkcnt;
}

/*
 * ------------------------------------------------------------
 * 接口实现 1: 获取分区信息
 * ------------------------------------------------------------
 */
int fastboot_spi_flash_get_part_info(const char *part_name,
				     struct disk_partition *part_info,
				     char *response)
{
	struct mtd_info *mtd;
	int ret;

	if (!part_name || !*part_name) {
		fastboot_fail("partition not given", response);
		return -ENOENT;
	}

	ret = get_mtd_device_by_name(part_name, &mtd);
	if (ret) {
		printf("Fastboot: Partition '%s' not found via MTD.\n", part_name);
		fastboot_fail("partition not found", response);
		return ret;
	}

	/* 伪造 disk_partition 结构以满足 fastboot 协议要求 */
	/* 注意：对于 MTD 分区，我们认为它的逻辑起始地址是 0 */
	part_info->start = 0;
	part_info->size = mtd->size;
	/* 使用 erasesize (Block Size) 还是 writesize (Page Size) ? 
	 * Sparse 镜像通常按块管理，但为了粒度更细，这里给 Page Size */
	part_info->blksz = mtd->writesize; 
	strncpy((char *)part_info->name, mtd->name, PART_NAME_LEN);

	/* 释放引用，防止计数器泄漏 */
	put_mtd_device(mtd);

	return 0;
}

/*
 * ------------------------------------------------------------
 * 接口实现 2: 擦除分区
 * ------------------------------------------------------------
 */
void fastboot_spi_flash_erase(const char *cmd, char *response)
{
	struct mtd_info *mtd;
	struct erase_info instr = {0};
	int ret;

	if (get_mtd_device_by_name(cmd, &mtd)) {
		fastboot_fail("partition not found", response);
		return;
	}

	printf("Erasing MTD partition '%s' (0x%llx bytes)...\n", mtd->name, mtd->size);

	instr.mtd = mtd;
	instr.addr = 0;
	instr.len = mtd->size; /* 擦除整个分区 */

	/* MTD 擦除调用 */
	ret = mtd_erase(mtd, &instr);
	
	put_mtd_device(mtd);

	if (ret) {
		printf("MTD Erase Failed: %d\n", ret);
		fastboot_fail("failed erasing mtd device", response);
	} else {
		fastboot_okay(NULL, response);
	}
}

/*
 * ------------------------------------------------------------
 * 接口实现 3: 写入镜像 (支持 Raw 和 Sparse)
 * ------------------------------------------------------------
 */
void fastboot_spi_flash_write(const char *cmd, void *download_buffer,
			      u32 download_bytes, char *response)
{
	struct mtd_info *mtd;
	int ret;

	if (get_mtd_device_by_name(cmd, &mtd)) {
		fastboot_fail("partition not found", response);
		return;
	}

	/* 1. 处理 Sparse Image (稀疏镜像) */
	if (is_sparse_image(download_buffer)) {
		struct sparse_storage sparse;

		printf("Flashing sparse image to '%s'...\n", mtd->name);

		sparse.blksz = mtd->writesize;
		sparse.start = 0;
		sparse.size = mtd->size / sparse.blksz;
		sparse.write = fb_mtd_sparse_write;
		sparse.reserve = fb_mtd_sparse_reserve;
		sparse.mssg = fastboot_fail;
		sparse.priv = mtd; /* 传递 mtd 对象给回调 */

		ret = write_sparse_image(&sparse, cmd, download_buffer, response);
		/* write_sparse_image 内部处理了 response，这里不需要再处理 */
	} 
	/* 2. 处理 Raw Image (原始二进制) */
	else {
		printf("Flashing raw image to '%s' (Size: %u)...\n", mtd->name, download_bytes);
		
		/* 
		 * 关键步骤：NAND 写入前必须擦除。
		 * 为了安全，我们只擦除我们要写入的那个区域。
		 * 计算对齐到 Block (Erasure) Size 的大小。
		 */
		struct erase_info instr = {0};
		size_t retlen;
		
		instr.mtd = mtd;
		instr.addr = 0;
		instr.len = download_bytes;

		/* 对齐长度到 erase_size (通常 128KB) */
		if (instr.len % mtd->erasesize) {
			instr.len = ((instr.len / mtd->erasesize) + 1) * mtd->erasesize;
		}
		
		/* 防止越界 */
		if (instr.len > mtd->size)
			instr.len = mtd->size;

		printf(" - Erasing 0x%llx bytes first...\n", instr.len);
		ret = mtd_erase(mtd, &instr);
		if (ret) {
			printf("Erase failed before write: %d\n", ret);
			fastboot_fail("erase failed", response);
			put_mtd_device(mtd);
			return;
		}

		printf(" - Writing data...\n");
		ret = mtd_write(mtd, 0, download_bytes, &retlen, download_buffer);
		if (ret) {
			printf("Write failed: %d\n", ret);
			fastboot_fail("write failed", response);
		} else {
			printf(" - Wrote %zu bytes.\n", retlen);
			fastboot_okay(NULL, response);
		}
	}

	put_mtd_device(mtd);
}

/* 
 * 占位函数，满足链接器对 weak symbol 的需求
 * 如果板级代码没有实现这些 setup 函数，这里提供空实现
 */
__weak int board_fastboot_spi_flash_write_setup(void) { return 0; }
__weak int board_fastboot_spi_flash_erase_setup(void) { return 0; }
