// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART (Device Address Resolution Table) IOMMU driver
 *
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * Based on arm/arm-smmu/arm-ssmu.c and arm/arm-smmu-v3/arm-smmu-v3.c
 *  Copyright (C) 2013 ARM Limited
 *  Copyright (C) 2015 ARM Limited
 * and on exynos-iommu.c
 *  Copyright (c) 2011,2016 Samsung Electronics Co., Ltd.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <linux/types.h>

#include "dma-iommu.h"

#define DART_MAX_STREAMS 256
#define DART_MAX_TTBR 4
#define MAX_DARTS_PER_DEVICE 3

/* Common registers */

#define DART_PARAMS1 0x00
#define DART_PARAMS1_PAGE_SHIFT GENMASK(27, 24)

#define DART_PARAMS2 0x04
#define DART_PARAMS2_BYPASS_SUPPORT BIT(0)

/* T8020/T6000 registers */

#define DART_T8020_STREAM_COMMAND 0x20
#define DART_T8020_STREAM_COMMAND_BUSY BIT(2)
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)

#define DART_T8020_STREAM_SELECT 0x34

#define DART_T8020_ERROR 0x40
#define DART_T8020_ERROR_STREAM GENMASK(27, 24)
#define DART_T8020_ERROR_CODE GENMASK(11, 0)
#define DART_T8020_ERROR_FLAG BIT(31)

#define DART_T8020_ERROR_READ_FAULT BIT(4)
#define DART_T8020_ERROR_WRITE_FAULT BIT(3)
#define DART_T8020_ERROR_NO_PTE BIT(2)
#define DART_T8020_ERROR_NO_PMD BIT(1)
#define DART_T8020_ERROR_NO_TTBR BIT(0)

#define DART_T8020_CONFIG 0x60
#define DART_T8020_CONFIG_LOCK BIT(15)

#define DART_STREAM_COMMAND_BUSY_TIMEOUT 100

#define DART_T8020_ERROR_ADDR_HI 0x54
#define DART_T8020_ERROR_ADDR_LO 0x50

#define DART_T8020_STREAMS_ENABLE 0xfc

#define DART_T8020_TCR                  0x100
#define DART_T8020_TCR_TRANSLATE_ENABLE BIT(7)
#define DART_T8020_TCR_BYPASS_DART      BIT(8)
#define DART_T8020_TCR_BYPASS_DAPF      BIT(12)

#define DART_T8020_TTBR       0x200
#define DART_T8020_USB4_TTBR  0x400
#define DART_T8020_TTBR_VALID BIT(31)
#define DART_T8020_TTBR_ADDR_FIELD_SHIFT 0
#define DART_T8020_TTBR_SHIFT 12

/* T8110 registers */

#define DART_T8110_PARAMS3 0x08
#define DART_T8110_PARAMS3_PA_WIDTH GENMASK(29, 24)
#define DART_T8110_PARAMS3_VA_WIDTH GENMASK(21, 16)
#define DART_T8110_PARAMS3_VER_MAJ GENMASK(15, 8)
#define DART_T8110_PARAMS3_VER_MIN GENMASK(7, 0)

#define DART_T8110_PARAMS4 0x0c
#define DART_T8110_PARAMS4_NUM_CLIENTS GENMASK(24, 16)
#define DART_T8110_PARAMS4_NUM_SIDS GENMASK(8, 0)

#define DART_T8110_TLB_CMD              0x80
#define DART_T8110_TLB_CMD_BUSY         BIT(31)
#define DART_T8110_TLB_CMD_OP           GENMASK(10, 8)
#define DART_T8110_TLB_CMD_OP_FLUSH_ALL 0
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
#define DART_T8110_TLB_CMD_STREAM       GENMASK(7, 0)

#define DART_T8110_ERROR 0x100
#define DART_T8110_ERROR_STREAM GENMASK(27, 20)
#define DART_T8110_ERROR_CODE GENMASK(14, 0)
#define DART_T8110_ERROR_FLAG BIT(31)

#define DART_T8110_ERROR_MASK 0x104

#define DART_T8110_ERROR_READ_FAULT BIT(5)
#define DART_T8110_ERROR_WRITE_FAULT BIT(4)
#define DART_T8110_ERROR_NO_PTE BIT(3)
#define DART_T8110_ERROR_NO_PMD BIT(2)
#define DART_T8110_ERROR_NO_PGD BIT(1)
#define DART_T8110_ERROR_NO_TTBR BIT(0)

#define DART_T8110_ERROR_ADDR_LO 0x170
#define DART_T8110_ERROR_ADDR_HI 0x174

#define DART_T8110_ERROR_STREAMS 0x1c0

#define DART_T8110_PROTECT 0x200
#define DART_T8110_UNPROTECT 0x204
#define DART_T8110_PROTECT_LOCK 0x208
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)

#define DART_T8110_ENABLE_STREAMS  0xc00
#define DART_T8110_DISABLE_STREAMS 0xc20

#define DART_T8110_TCR                  0x1000
#define DART_T8110_TCR_REMAP            GENMASK(11, 8)
#define DART_T8110_TCR_REMAP_EN         BIT(7)
#define DART_T8110_TCR_FOUR_LEVEL       BIT(3)
#define DART_T8110_TCR_BYPASS_DAPF      BIT(2)
#define DART_T8110_TCR_BYPASS_DART      BIT(1)
#define DART_T8110_TCR_TRANSLATE_ENABLE BIT(0)

#define DART_T8110_TTBR       0x1400
#define DART_T8110_TTBR_VALID BIT(0)
#define DART_T8110_TTBR_ADDR_FIELD_SHIFT 2
#define DART_T8110_TTBR_SHIFT 14

#define DART_TCR(dart, sid) ((dart)->hw->tcr + ((sid) << 2))

#define DART_TTBR(dart, sid, idx) ((dart)->hw->ttbr + \
				   (((dart)->hw->ttbr_count * (sid)) << 2) + \
				   ((idx) << 2))

struct apple_dart_stream_map;

enum dart_type {
	DART_T8020,
	DART_T6000,
	DART_T8110,
};

struct apple_dart_hw {
	enum dart_type type;
	irqreturn_t (*irq_handler)(int irq, void *dev);
	int (*invalidate_tlb)(struct apple_dart_stream_map *stream_map);

	u32 oas;
	enum io_pgtable_fmt fmt;

	int max_sid_count;

	u32 lock;
	u32 lock_bit;

	u32 error;

	u32 enable_streams;

	u32 tcr;
	u32 tcr_enabled;
	u32 tcr_disabled;
	u32 tcr_bypass;
	u32 tcr_4level;

	u32 ttbr;
	u32 ttbr_valid;
	u32 ttbr_addr_field_shift;
	u32 ttbr_shift;
	int ttbr_count;
};

/*
 * Private structure associated with each DART device.
 *
 * @dev: device struct
 * @hw: SoC-specific hardware data
 * @regs: mapped MMIO region
 * @irq: interrupt number, can be shared with other DARTs
 * @clks: clocks associated with this DART
 * @num_clks: number of @clks
 * @lock: lock for hardware operations involving this dart
 * @pgsize: pagesize supported by this DART
 * @supports_bypass: indicates if this DART supports bypass mode
 * @locked: indicates if this DART is locked
 * @sid2group: maps stream ids to iommu_groups
 * @iommu: iommu core device
 */
struct apple_dart {
	struct device *dev;
	const struct apple_dart_hw *hw;

	void __iomem *regs;

	int irq;
	struct clk_bulk_data *clks;
	int num_clks;

	spinlock_t lock;

	u32 ias;
	u32 oas;
	u32 pgsize;
	u32 num_streams;
	u32 supports_bypass : 1;
	u32 locked : 1;
	u32 four_level : 1;

	dma_addr_t dma_min;
	dma_addr_t dma_max;

	struct iommu_group *sid2group[DART_MAX_STREAMS];
	struct iommu_device iommu;

	u32 save_tcr[DART_MAX_STREAMS];
	u32 save_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];

	u64 *locked_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
	u64 *shadow_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
};

/*
 * Convenience struct to identify streams.
 *
 * The normal variant is used inside apple_dart_master_cfg which isn't written
 * to concurrently.
 * The atomic variant is used inside apple_dart_domain where we have to guard
 * against races from potential parallel calls to attach/detach_device.
 * Note that even inside the atomic variant the apple_dart pointer is not
 * protected: This pointer is initialized once under the domain init mutex
 * and never changed again afterwards. Devices with different dart pointers
 * cannot be attached to the same domain.
 *
 * @dart dart pointer
 * @sid stream id bitmap
 */
struct apple_dart_stream_map {
	struct apple_dart *dart;
	DECLARE_BITMAP(sidmap, DART_MAX_STREAMS);
};
struct apple_dart_atomic_stream_map {
	struct apple_dart *dart;
	atomic_long_t sidmap[BITS_TO_LONGS(DART_MAX_STREAMS)];
};

/*
 * This structure is attached to each iommu domain handled by a DART.
 *
 * @pgtbl_ops: pagetable ops allocated by io-pgtable
 * @finalized: true if the domain has been completely initialized
 * @init_lock: protects domain initialization
 * @stream_maps: streams attached to this domain (valid for DMA/UNMANAGED only)
 * @domain: core iommu domain pointer
 */
struct apple_dart_domain {
	struct io_pgtable_ops *pgtbl_ops;

	bool finalized;
	u64 mask;
	struct mutex init_lock;
	struct apple_dart_atomic_stream_map stream_maps[MAX_DARTS_PER_DEVICE];

	struct iommu_domain domain;
};

/*
 * This structure is attached to devices with dev_iommu_priv_set() on of_xlate
 * and contains a list of streams bound to this device.
 * So far the worst case seen is a single device with two streams
 * from different darts, such that this simple static array is enough.
 *
 * @streams: streams for this device
 */
struct apple_dart_master_cfg {
	/* Union of DART capabilitles */
	u32 supports_bypass : 1;

	struct apple_dart_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
};

/*
 * Helper macro to iterate over apple_dart_master_cfg.stream_maps and
 * apple_dart_domain.stream_maps
 *
 * @i int used as loop variable
 * @base pointer to base struct (apple_dart_master_cfg or apple_dart_domain)
 * @stream pointer to the apple_dart_streams struct for each loop iteration
 */
#define for_each_stream_map(i, base, stream_map)                               \
	for (i = 0, stream_map = &(base)->stream_maps[0];                      \
	     i < MAX_DARTS_PER_DEVICE && stream_map->dart;                     \
	     stream_map = &(base)->stream_maps[++i])

static struct platform_driver apple_dart_driver;
static const struct iommu_ops apple_dart_iommu_ops;

static struct apple_dart_domain *to_dart_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_domain, domain);
}

static void
apple_dart_hw_enable_translation(struct apple_dart_stream_map *stream_map, int levels)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(levels != 3 && levels != 4);
	WARN_ON(levels == 4 && !dart->four_level);
	WARN_ON(stream_map->dart->locked);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->tcr_enabled | (levels == 4 ? dart->hw->tcr_4level : 0),
		       dart->regs + DART_TCR(dart, sid));
}

static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(stream_map->dart->locked);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->tcr_disabled, dart->regs + DART_TCR(dart, sid));
}

static void
apple_dart_hw_enable_bypass(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(stream_map->dart->locked);
	WARN_ON(!stream_map->dart->supports_bypass);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->tcr_bypass,
		       dart->regs + DART_TCR(dart, sid));
}

static void apple_dart_hw_set_ttbr(struct apple_dart_stream_map *stream_map,
				   u8 idx, phys_addr_t paddr)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(stream_map->dart->locked);
	WARN_ON(paddr & ((1 << dart->hw->ttbr_shift) - 1));
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->ttbr_valid |
		       (paddr >> dart->hw->ttbr_shift) << dart->hw->ttbr_addr_field_shift,
		       dart->regs + DART_TTBR(dart, sid, idx));
}

static void apple_dart_hw_clear_ttbr(struct apple_dart_stream_map *stream_map,
				     u8 idx)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(stream_map->dart->locked);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(0, dart->regs + DART_TTBR(dart, sid, idx));
}

static void
apple_dart_hw_clear_all_ttbrs(struct apple_dart_stream_map *stream_map)
{
	int i;

	for (i = 0; i < stream_map->dart->hw->ttbr_count; ++i)
		apple_dart_hw_clear_ttbr(stream_map, i);
}

static int
apple_dart_hw_set_locked_ttbr(struct apple_dart_stream_map *stream_map, u8 idx,
			      phys_addr_t paddr)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(!dart->locked);
	WARN_ON(paddr & ((1 << dart->hw->ttbr_shift) - 1));
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		u32 ttbr;
		phys_addr_t phys;
		u64 *l1_tbl, *l1_shadow;

		ttbr = readl(dart->regs + DART_TTBR(dart, sid, idx));

		WARN_ON(!(ttbr & dart->hw->ttbr_valid));
		ttbr &= ~dart->hw->ttbr_valid;

		if (dart->hw->ttbr_addr_field_shift)
			ttbr >>= dart->hw->ttbr_addr_field_shift;
		phys = ((phys_addr_t) ttbr) << dart->hw->ttbr_shift;

		l1_tbl = devm_memremap(dart->dev, phys, dart->pgsize,
				       MEMREMAP_WB);
		if (!l1_tbl)
			return -ENOMEM;
		l1_shadow = devm_memremap(dart->dev, paddr, dart->pgsize,
				       MEMREMAP_WB);
		if (!l1_shadow)
			return -ENOMEM;

		dart->locked_ttbr[sid][idx] = l1_tbl;
		dart->shadow_ttbr[sid][idx] = l1_shadow;
	}

	return 0;
}

static int
apple_dart_hw_clear_locked_ttbr(struct apple_dart_stream_map *stream_map,
				u8 idx)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(!dart->locked);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		/* TODO: locked L1 table might need to be restored to boot state */
		if (dart->locked_ttbr[sid][idx]) {
			memset(dart->locked_ttbr[sid][idx], 0, dart->pgsize);
			devm_memunmap(dart->dev, dart->locked_ttbr[sid][idx]);
		}
		dart->locked_ttbr[sid][idx] = NULL;
		if (dart->shadow_ttbr[sid][idx])
			devm_memunmap(dart->dev, dart->shadow_ttbr[sid][idx]);
		dart->shadow_ttbr[sid][idx] = NULL;
	}

	return 0;
}

static int
apple_dart_hw_sync_locked(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(!dart->locked);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		for (int idx = 0; idx < dart->hw->ttbr_count; idx++) {
			u64 *ttbrep = dart->locked_ttbr[sid][idx];
			u64 *ptep = dart->shadow_ttbr[sid][idx];
			if (!ttbrep || !ptep)
				continue;
			for (int entry = 0; entry < dart->pgsize / sizeof(*ptep); entry++)
				ttbrep[entry] = ptep[entry];
		}
	}

	return 0;
}

static int
apple_dart_t8020_hw_stream_command(struct apple_dart_stream_map *stream_map,
			     u32 command)
{
	unsigned long flags;
	int ret, i;
	u32 command_reg;

	spin_lock_irqsave(&stream_map->dart->lock, flags);

	for (i = 0; i < BITS_TO_U32(stream_map->dart->num_streams); i++)
		writel(stream_map->sidmap[i],
		       stream_map->dart->regs + DART_T8020_STREAM_SELECT + 4 * i);
	writel(command, stream_map->dart->regs + DART_T8020_STREAM_COMMAND);

	ret = readl_poll_timeout_atomic(
		stream_map->dart->regs + DART_T8020_STREAM_COMMAND, command_reg,
		!(command_reg & DART_T8020_STREAM_COMMAND_BUSY), 1,
		DART_STREAM_COMMAND_BUSY_TIMEOUT);

	spin_unlock_irqrestore(&stream_map->dart->lock, flags);

	if (ret) {
		dev_err(stream_map->dart->dev,
			"busy bit did not clear after command %x for streams %lx\n",
			command, stream_map->sidmap[0]);
		return ret;
	}

	return 0;
}

static int
apple_dart_t8110_hw_tlb_command(struct apple_dart_stream_map *stream_map,
				u32 command)
{
	struct apple_dart *dart = stream_map->dart;
	unsigned long flags;
	int ret = 0;
	int sid;

	spin_lock_irqsave(&dart->lock, flags);

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		u32 val = FIELD_PREP(DART_T8110_TLB_CMD_OP, command) |
			FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid);
		writel(val, dart->regs + DART_T8110_TLB_CMD);

		ret = readl_poll_timeout_atomic(
			dart->regs + DART_T8110_TLB_CMD, val,
			!(val & DART_T8110_TLB_CMD_BUSY), 1,
			DART_STREAM_COMMAND_BUSY_TIMEOUT);

		if (ret)
			break;

	}

	spin_unlock_irqrestore(&dart->lock, flags);

	if (ret) {
		dev_err(stream_map->dart->dev,
			"busy bit did not clear after command %x for stream %d\n",
			command, sid);
		return ret;
	}

	return 0;
}

static int
apple_dart_t8020_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	return apple_dart_t8020_hw_stream_command(
		stream_map, DART_T8020_STREAM_COMMAND_INVALIDATE);
}

static int
apple_dart_t8110_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	return apple_dart_t8110_hw_tlb_command(
		stream_map, DART_T8110_TLB_CMD_OP_FLUSH_SID);
}

static int apple_dart_hw_reset(struct apple_dart *dart)
{
	struct apple_dart_stream_map stream_map;
	int i;

	stream_map.dart = dart;
	bitmap_zero(stream_map.sidmap, DART_MAX_STREAMS);
	bitmap_set(stream_map.sidmap, 0, dart->num_streams);
	apple_dart_hw_disable_dma(&stream_map);
	apple_dart_hw_clear_all_ttbrs(&stream_map);

	/* enable all streams globally since TCR is used to control isolation */
	for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		writel(U32_MAX, dart->regs + dart->hw->enable_streams + 4 * i);

	/* clear any pending errors before the interrupt is unmasked */
	writel(readl(dart->regs + dart->hw->error), dart->regs + dart->hw->error);

	if (dart->hw->type == DART_T8110)
		writel(0,  dart->regs + DART_T8110_ERROR_MASK);

	return dart->hw->invalidate_tlb(&stream_map);
}

static void apple_dart_domain_flush_tlb(struct apple_dart_domain *domain)
{
	int i, j;
	struct apple_dart_atomic_stream_map *domain_stream_map;
	struct apple_dart_stream_map stream_map;

	for_each_stream_map(i, domain, domain_stream_map) {
		stream_map.dart = domain_stream_map->dart;

		for (j = 0; j < BITS_TO_LONGS(stream_map.dart->num_streams); j++)
			stream_map.sidmap[j] = atomic_long_read(&domain_stream_map->sidmap[j]);

		WARN_ON(pm_runtime_get_sync(stream_map.dart->dev) < 0);

		if (stream_map.dart->locked)
			apple_dart_hw_sync_locked(&stream_map);

		stream_map.dart->hw->invalidate_tlb(&stream_map);
		pm_runtime_put(stream_map.dart->dev);
	}
}

static void apple_dart_flush_iotlb_all(struct iommu_domain *domain)
{
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
}

static void apple_dart_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
}

static int apple_dart_iotlb_sync_map(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
	return 0;
}

static phys_addr_t apple_dart_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	if (!ops)
		return 0;

	return ops->iova_to_phys(ops, iova & dart_domain->mask);
}

static int apple_dart_map_pages(struct iommu_domain *domain, unsigned long iova,
				phys_addr_t paddr, size_t pgsize,
				size_t pgcount, int prot, gfp_t gfp,
				size_t *mapped)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	if (!ops)
		return -ENODEV;

	return ops->map_pages(ops, iova & dart_domain->mask, paddr, pgsize,
			      pgcount, prot, gfp, mapped);
}

static size_t apple_dart_unmap_pages(struct iommu_domain *domain,
				     unsigned long iova, size_t pgsize,
				     size_t pgcount,
				     struct iommu_iotlb_gather *gather)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	return ops->unmap_pages(ops, iova & dart_domain->mask, pgsize, pgcount,
				gather);
}

static void
apple_dart_setup_translation(struct apple_dart_domain *domain,
			     struct apple_dart_stream_map *stream_map)
{
	int i;
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(domain->pgtbl_ops)->cfg;

	/* Locked DARTs are set up by the bootloader. */
	if (stream_map->dart->locked) {
		for (i = 0; i < pgtbl_cfg->apple_dart_cfg.n_ttbrs; ++i)
			apple_dart_hw_set_locked_ttbr(stream_map, i,
					pgtbl_cfg->apple_dart_cfg.ttbr[i]);
		for (; i < stream_map->dart->hw->ttbr_count; ++i)
			apple_dart_hw_clear_locked_ttbr(stream_map, i);
		apple_dart_hw_sync_locked(stream_map);
	} else {
		for (i = 0; i < pgtbl_cfg->apple_dart_cfg.n_ttbrs; ++i)
			apple_dart_hw_set_ttbr(stream_map, i,
					pgtbl_cfg->apple_dart_cfg.ttbr[i]);
		for (; i < stream_map->dart->hw->ttbr_count; ++i)
			apple_dart_hw_clear_ttbr(stream_map, i);

		apple_dart_hw_enable_translation(stream_map,
						 pgtbl_cfg->apple_dart_cfg.n_levels);
	}
	stream_map->dart->hw->invalidate_tlb(stream_map);
}

static int apple_dart_setup_resv_locked(struct iommu_domain *domain,
					struct device *dev, size_t pgsize)
{
	struct iommu_resv_region *region;
	LIST_HEAD(resv_regions);
	int ret = 0;

	of_iommu_get_resv_regions(dev, &resv_regions);
	list_for_each_entry(region, &resv_regions, list) {
		size_t mapped = 0;

		/* Only map translated reserved regions */
		if (region->type != IOMMU_RESV_TRANSLATED)
			continue;

		while (mapped < region->length) {
			phys_addr_t paddr = region->start + mapped;
			unsigned long iova = region->dva + mapped;
			size_t length = region->length - mapped;
			size_t pgcount = length / pgsize;

			ret = apple_dart_map_pages(domain, iova,
			      paddr, pgsize, pgcount,
			      region->prot, GFP_KERNEL, &mapped);

			if (ret)
				goto end_put;
		}
	}
end_put:
	iommu_put_resv_regions(dev, &resv_regions);
	return ret;
}

static int apple_dart_finalize_domain(struct apple_dart_domain *dart_domain,
				      struct device *dev,
				      struct apple_dart_master_cfg *cfg)
{
	struct apple_dart *dart = cfg->stream_maps[0].dart;
	struct io_pgtable_cfg pgtbl_cfg;
	dma_addr_t dma_max = dart->dma_max;
	u32 ias = min_t(u32, dart->ias, fls64(dma_max));
	int ret = 0;
	int i, j;

	if (dart->pgsize > PAGE_SIZE)
		return -EINVAL;

	mutex_lock(&dart_domain->init_lock);

	if (dart_domain->finalized)
		goto done;

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		dart_domain->stream_maps[i].dart = cfg->stream_maps[i].dart;
		for (j = 0; j < BITS_TO_LONGS(dart->num_streams); j++)
			atomic_long_set(&dart_domain->stream_maps[i].sidmap[j],
					cfg->stream_maps[i].sidmap[j]);
	}

	pgtbl_cfg = (struct io_pgtable_cfg){
		.pgsize_bitmap = dart->pgsize,
		.ias = ias,
		.oas = dart->oas,
		.coherent_walk = 1,
		.iommu_dev = dart->dev,
	};

	if (dart->locked) {
		unsigned long *sidmap;
		int sid;
		u32 ttbr;

		/* Locked DARTs can only have a single stream bound */
		sidmap = cfg->stream_maps[0].sidmap;
		sid = find_first_bit(sidmap, dart->num_streams);

		WARN_ON((sid < 0) || bitmap_weight(sidmap, dart->num_streams) > 1);
		ttbr = readl(dart->regs + DART_TTBR(dart, sid, 0));

		WARN_ON(!(ttbr & dart->hw->ttbr_valid));

		/* If the DART is locked, we need to keep the translation level count. */
		if (dart->hw->tcr_4level && dart->ias > 36) {
			if (readl(dart->regs + DART_TCR(dart, sid)) & dart->hw->tcr_4level) {
				if (ias < 37) {
					dev_info(dart->dev, "Expanded to ias=37 due to lock\n");
					pgtbl_cfg.ias = 37;
				}
			} else if (ias > 36) {
				dev_info(dart->dev, "Limited to ias=36 due to lock\n");
				pgtbl_cfg.ias = 36;
				if (dart->dma_min == 0 && dma_max == DMA_BIT_MASK(dart->ias)) {
					dma_max = DMA_BIT_MASK(pgtbl_cfg.ias);
				} else if ((dart->dma_min ^ dma_max) & ~DMA_BIT_MASK(36)) {
					dev_err(dart->dev,
						"Invalid DMA range for locked 3-level PT\n");
					ret = -ENOMEM;
					goto done;
				}
			}
		}
	}

	dart_domain->pgtbl_ops = alloc_io_pgtable_ops(dart->hw->fmt, &pgtbl_cfg,
						      &dart_domain->domain);
	if (!dart_domain->pgtbl_ops) {
		ret = -ENOMEM;
		goto done;
	}

	if (pgtbl_cfg.pgsize_bitmap == SZ_4K)
		dart_domain->mask = DMA_BIT_MASK(min_t(u32, dart->ias, 32));
	else if (pgtbl_cfg.apple_dart_cfg.n_levels == 3)
		dart_domain->mask = DMA_BIT_MASK(min_t(u32, dart->ias, 36));
	else if (pgtbl_cfg.apple_dart_cfg.n_levels == 4)
		dart_domain->mask = DMA_BIT_MASK(min_t(u32, dart->ias, 47));

	dart_domain->domain.pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	dart_domain->domain.geometry.aperture_start = dart->dma_min;
	dart_domain->domain.geometry.aperture_end = dma_max;
	dart_domain->domain.geometry.force_aperture = true;

	dart_domain->finalized = true;

	ret = apple_dart_setup_resv_locked(&dart_domain->domain, dev, dart->pgsize);
done:
	mutex_unlock(&dart_domain->init_lock);
	return ret;
}

static int
apple_dart_mod_streams(struct apple_dart_atomic_stream_map *domain_maps,
		       struct apple_dart_stream_map *master_maps,
		       bool add_streams)
{
	int i, j;

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (domain_maps[i].dart != master_maps[i].dart)
			return -EINVAL;
	}

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (!domain_maps[i].dart)
			break;
		for (j = 0; j < BITS_TO_LONGS(domain_maps[i].dart->num_streams); j++) {
			if (add_streams)
				atomic_long_or(master_maps[i].sidmap[j],
					       &domain_maps[i].sidmap[j]);
			else
				atomic_long_and(~master_maps[i].sidmap[j],
						&domain_maps[i].sidmap[j]);
		}
	}

	return 0;
}

static int apple_dart_domain_add_streams(struct apple_dart_domain *domain,
					 struct apple_dart_master_cfg *cfg)
{
	return apple_dart_mod_streams(domain->stream_maps, cfg->stream_maps,
				      true);
}

static int apple_dart_attach_dev_paging(struct iommu_domain *domain,
					struct device *dev)
{
	int ret, i;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	ret = apple_dart_finalize_domain(dart_domain, dev, cfg);
	if (ret)
		goto err;

	ret = apple_dart_domain_add_streams(dart_domain, cfg);
	if (ret)
		goto err;

	for_each_stream_map(i, cfg, stream_map)
		apple_dart_setup_translation(dart_domain, stream_map);
err:
	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return ret;
}

static int apple_dart_attach_dev_identity(struct iommu_domain *domain,
					  struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	if (!cfg->supports_bypass)
		return -EINVAL;

	if (cfg->stream_maps[0].dart->locked)
		return -EINVAL;

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	for_each_stream_map(i, cfg, stream_map)
		apple_dart_hw_enable_bypass(stream_map);

	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return 0;
}

static const struct iommu_domain_ops apple_dart_identity_ops = {
	.attach_dev = apple_dart_attach_dev_identity,
};

static struct iommu_domain apple_dart_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &apple_dart_identity_ops,
};

static int apple_dart_attach_dev_blocked(struct iommu_domain *domain,
					 struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	for_each_stream_map(i, cfg, stream_map)
		apple_dart_hw_disable_dma(stream_map);

	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return 0;
}

static const struct iommu_domain_ops apple_dart_blocked_ops = {
	.attach_dev = apple_dart_attach_dev_blocked,
};

static struct iommu_domain apple_dart_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &apple_dart_blocked_ops,
};

static struct iommu_device *apple_dart_probe_device(struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	if (!dev_iommu_fwspec_get(dev) || !cfg)
		return ERR_PTR(-ENODEV);

	for_each_stream_map(i, cfg, stream_map)
		device_link_add(dev, stream_map->dart->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER |
			DL_FLAG_RPM_ACTIVE);

	return &cfg->stream_maps[0].dart->iommu;
}

static void apple_dart_release_device(struct device *dev)
{
	int i, j;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	for_each_stream_map(j, cfg, stream_map) {
		if (stream_map->dart->locked)
			for (i = 0; i < stream_map->dart->hw->ttbr_count; ++i)
				apple_dart_hw_clear_locked_ttbr(stream_map, i);
	}

	kfree(cfg);
}

static struct iommu_domain *apple_dart_domain_alloc_paging(struct device *dev)
{
	struct apple_dart_domain *dart_domain;

	dart_domain = kzalloc(sizeof(*dart_domain), GFP_KERNEL);
	if (!dart_domain)
		return NULL;

	mutex_init(&dart_domain->init_lock);

	if (dev) {
		struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
		int ret;

		ret = apple_dart_finalize_domain(dart_domain, dev, cfg);
		if (ret) {
			kfree(dart_domain);
			return ERR_PTR(ret);
		}
	}
	return &dart_domain->domain;
}

static void apple_dart_domain_free(struct iommu_domain *domain)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);

	if (dart_domain->pgtbl_ops)
		free_io_pgtable_ops(dart_domain->pgtbl_ops);

	kfree(dart_domain);
}

static int apple_dart_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct platform_device *iommu_pdev = of_find_device_by_node(args->np);
	struct apple_dart *dart = platform_get_drvdata(iommu_pdev);
	struct apple_dart *cfg_dart;
	int i, sid;

	if (args->args_count != 1)
		return -EINVAL;
	sid = args->args[0];

	if (!cfg) {
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);

		/* Will be ANDed with DART capabilities */
		cfg->supports_bypass = true;
	}
	if (!cfg)
		return -ENOMEM;
	dev_iommu_priv_set(dev, cfg);

	cfg_dart = cfg->stream_maps[0].dart;
	if (cfg_dart) {
		if (cfg_dart->pgsize != dart->pgsize)
			return -EINVAL;
	}

	if (!dart->supports_bypass)
		cfg->supports_bypass = false;

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (cfg->stream_maps[i].dart == dart) {
			set_bit(sid, cfg->stream_maps[i].sidmap);
			return 0;
		}
	}
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (!cfg->stream_maps[i].dart) {
			cfg->stream_maps[i].dart = dart;
			set_bit(sid, cfg->stream_maps[i].sidmap);
			return 0;
		}
	}

	return -EINVAL;
}

static DEFINE_MUTEX(apple_dart_groups_lock);

static void apple_dart_release_group(void *iommu_data)
{
	int i, sid;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *group_master_cfg = iommu_data;

	mutex_lock(&apple_dart_groups_lock);

	for_each_stream_map(i, group_master_cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = NULL;

	kfree(iommu_data);
	mutex_unlock(&apple_dart_groups_lock);
}

static int apple_dart_merge_master_cfg(struct apple_dart_master_cfg *dst,
				       struct apple_dart_master_cfg *src)
{
	/*
	 * We know that this function is only called for groups returned from
	 * pci_device_group and that all Apple Silicon platforms never spread
	 * PCIe devices from the same bus across multiple DARTs such that we can
	 * just assume that both src and dst only have the same single DART.
	 */
	if (src->stream_maps[1].dart)
		return -EINVAL;
	if (dst->stream_maps[1].dart)
		return -EINVAL;
	if (src->stream_maps[0].dart != dst->stream_maps[0].dart)
		return -EINVAL;

	bitmap_or(dst->stream_maps[0].sidmap,
		  dst->stream_maps[0].sidmap,
		  src->stream_maps[0].sidmap,
		  dst->stream_maps[0].dart->num_streams);
	return 0;
}

static struct iommu_group *apple_dart_device_group(struct device *dev)
{
	int i, sid;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *group_master_cfg;
	struct iommu_group *group = NULL;
	struct iommu_group *res = ERR_PTR(-EINVAL);

	mutex_lock(&apple_dart_groups_lock);

	for_each_stream_map(i, cfg, stream_map) {
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams) {
			struct iommu_group *stream_group =
				stream_map->dart->sid2group[sid];

			if (group && group != stream_group) {
				res = ERR_PTR(-EINVAL);
				goto out;
			}

			group = stream_group;
		}
	}

	if (group) {
		res = iommu_group_ref_get(group);
		goto out;
	}

#ifdef CONFIG_PCI
	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
#endif
		group = generic_device_group(dev);

	res = ERR_PTR(-ENOMEM);
	if (!group)
		goto out;

	group_master_cfg = iommu_group_get_iommudata(group);
	if (group_master_cfg) {
		int ret;

		ret = apple_dart_merge_master_cfg(group_master_cfg, cfg);
		if (ret) {
			dev_err(dev, "Failed to merge DART IOMMU groups.\n");
			iommu_group_put(group);
			res = ERR_PTR(ret);
			goto out;
		}
	} else {
		group_master_cfg = kmemdup(cfg, sizeof(*group_master_cfg),
					   GFP_KERNEL);
		if (!group_master_cfg) {
			iommu_group_put(group);
			goto out;
		}

		iommu_group_set_iommudata(group, group_master_cfg,
			apple_dart_release_group);
	}

	for_each_stream_map(i, cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = group;

	res = group;

out:
	mutex_unlock(&apple_dart_groups_lock);
	return res;
}

static int apple_dart_def_domain_type(struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	if (cfg->stream_maps[0].dart->pgsize > PAGE_SIZE)
		return IOMMU_DOMAIN_IDENTITY;
	if (!cfg->supports_bypass)
		return IOMMU_DOMAIN_DMA;
	if (cfg->stream_maps[0].dart->locked)
		return IOMMU_DOMAIN_DMA;

	return 0;
}

#ifndef CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR
/* Keep things compiling when CONFIG_PCI_APPLE isn't selected */
#define CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR	0
#endif
#define DOORBELL_ADDR	(CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR & PAGE_MASK)

static void apple_dart_get_resv_regions(struct device *dev,
					struct list_head *head)
{
	if (IS_ENABLED(CONFIG_PCIE_APPLE) && dev_is_pci(dev)) {
		struct iommu_resv_region *region;
		int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

		region = iommu_alloc_resv_region(DOORBELL_ADDR,
						 PAGE_SIZE, prot,
						 IOMMU_RESV_MSI, GFP_KERNEL);
		if (!region)
			return;

		list_add_tail(&region->list, head);
	}

	iommu_dma_get_resv_regions(dev, head);
}

static const struct iommu_ops apple_dart_iommu_ops = {
	.identity_domain = &apple_dart_identity_domain,
	.blocked_domain = &apple_dart_blocked_domain,
	.domain_alloc_paging = apple_dart_domain_alloc_paging,
	.probe_device = apple_dart_probe_device,
	.release_device = apple_dart_release_device,
	.device_group = apple_dart_device_group,
	.of_xlate = apple_dart_of_xlate,
	.def_domain_type = apple_dart_def_domain_type,
	.get_resv_regions = apple_dart_get_resv_regions,
	.pgsize_bitmap = -1UL, /* Restricted during dart probe */
	.owner = THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= apple_dart_attach_dev_paging,
		.map_pages	= apple_dart_map_pages,
		.unmap_pages	= apple_dart_unmap_pages,
		.flush_iotlb_all = apple_dart_flush_iotlb_all,
		.iotlb_sync	= apple_dart_iotlb_sync,
		.iotlb_sync_map	= apple_dart_iotlb_sync_map,
		.iova_to_phys	= apple_dart_iova_to_phys,
		.free		= apple_dart_domain_free,
	}
};

static irqreturn_t apple_dart_t8020_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;
	const char *fault_name = NULL;
	u32 error = readl(dart->regs + DART_T8020_ERROR);
	u32 error_code = FIELD_GET(DART_T8020_ERROR_CODE, error);
	u32 addr_lo = readl(dart->regs + DART_T8020_ERROR_ADDR_LO);
	u32 addr_hi = readl(dart->regs + DART_T8020_ERROR_ADDR_HI);
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	u8 stream_idx = FIELD_GET(DART_T8020_ERROR_STREAM, error);

	if (!(error & DART_T8020_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	if (error_code == DART_T8020_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	else if (error_code == DART_T8020_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	else if (error_code == DART_T8020_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	else if (error_code == DART_T8020_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	else if (error_code == DART_T8020_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		fault_name = "unknown";

	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	writel(error, dart->regs + DART_T8020_ERROR);
	return IRQ_HANDLED;
}

static irqreturn_t apple_dart_t8110_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;
	const char *fault_name = NULL;
	u32 error = readl(dart->regs + DART_T8110_ERROR);
	u32 error_code = FIELD_GET(DART_T8110_ERROR_CODE, error);
	u32 addr_lo = readl(dart->regs + DART_T8110_ERROR_ADDR_LO);
	u32 addr_hi = readl(dart->regs + DART_T8110_ERROR_ADDR_HI);
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	u8 stream_idx = FIELD_GET(DART_T8110_ERROR_STREAM, error);
	int i;

	if (!(error & DART_T8110_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	if (error_code == DART_T8110_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	else if (error_code == DART_T8110_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	else if (error_code == DART_T8110_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PGD)
		fault_name = "NO PGD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		fault_name = "unknown";

	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	writel(error, dart->regs + DART_T8110_ERROR);
	for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		writel(U32_MAX, dart->regs + DART_T8110_ERROR_STREAMS + 4 * i);

	return IRQ_HANDLED;
}

static irqreturn_t apple_dart_irq(int irq, void *dev)
{
	irqreturn_t ret;
	struct apple_dart *dart = dev;

	WARN_ON(pm_runtime_get_sync(dart->dev) < 0);
	ret = dart->hw->irq_handler(irq, dev);
	pm_runtime_put(dart->dev);
	return ret;
}

static bool apple_dart_is_locked(struct apple_dart *dart)
{
	return !!(readl(dart->regs + dart->hw->lock) & dart->hw->lock_bit);
}

static int apple_dart_probe(struct platform_device *pdev)
{
	int ret;
	u32 dart_params[4];
	struct resource *res;
	struct apple_dart *dart;
	struct device *dev = &pdev->dev;
	u64 dma_range[2];

	dart = devm_kzalloc(dev, sizeof(*dart), GFP_KERNEL);
	if (!dart)
		return -ENOMEM;

	dart->dev = dev;
	dart->hw = of_device_get_match_data(dev);
	spin_lock_init(&dart->lock);

	dart->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dart->regs))
		return PTR_ERR(dart->regs);

	if (resource_size(res) < 0x4000) {
		dev_err(dev, "MMIO region too small (%pr)\n", res);
		return -EINVAL;
	}

	dart->irq = platform_get_irq(pdev, 0);
	if (dart->irq < 0)
		return -ENODEV;

	ret = devm_clk_bulk_get_all(dev, &dart->clks);
	if (ret < 0)
		return ret;
	dart->num_clks = ret;

	ret = clk_bulk_prepare_enable(dart->num_clks, dart->clks);
	if (ret)
		return ret;

	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_irq_safe(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_clk_disable;

	dart_params[0] = readl(dart->regs + DART_PARAMS1);
	dart_params[1] = readl(dart->regs + DART_PARAMS2);
	dart->pgsize = 1 << FIELD_GET(DART_PARAMS1_PAGE_SHIFT, dart_params[0]);
	dart->supports_bypass = dart_params[1] & DART_PARAMS2_BYPASS_SUPPORT;

	switch (dart->hw->type) {
	case DART_T8020:
	case DART_T6000:
		dart->ias = 32;
		dart->oas = dart->hw->oas;
		dart->num_streams = dart->hw->max_sid_count;
		break;

	case DART_T8110:
		dart_params[2] = readl(dart->regs + DART_T8110_PARAMS3);
		dart_params[3] = readl(dart->regs + DART_T8110_PARAMS4);
		dart->ias = FIELD_GET(DART_T8110_PARAMS3_VA_WIDTH, dart_params[2]);
		dart->oas = FIELD_GET(DART_T8110_PARAMS3_PA_WIDTH, dart_params[2]);
		dart->num_streams = FIELD_GET(DART_T8110_PARAMS4_NUM_SIDS, dart_params[3]);
		dart->four_level = dart->ias > 36;
		break;
	}

	dart->dma_min = 0;
	dart->dma_max = DMA_BIT_MASK(dart->ias);

	ret = of_property_read_u64_array(dev->of_node, "apple,dma-range", dma_range, 2);
	if (ret == -EINVAL) {
		ret = 0;
	} else if (ret) {
		goto err_clk_disable;
	} else {
		dart->dma_min = dma_range[0];
		dart->dma_max = dma_range[0] + dma_range[1] - 1;
		if ((dart->dma_min ^ dart->dma_max) & ~DMA_BIT_MASK(dart->ias)) {
			dev_err(&pdev->dev, "Invalid DMA range for ias=%d\n",
				dart->ias);
			goto err_clk_disable;
		}
		dev_info(&pdev->dev, "Limiting DMA range to %pad..%pad\n",
			 &dart->dma_min, &dart->dma_max);
	}

	if (dart->num_streams > DART_MAX_STREAMS) {
		dev_err(&pdev->dev, "Too many streams (%d > %d)\n",
			dart->num_streams, DART_MAX_STREAMS);
		ret = -EINVAL;
		goto err_clk_disable;
	}

	dart->locked = apple_dart_is_locked(dart);
	if (!dart->locked) {
		ret = apple_dart_hw_reset(dart);
		if (ret)
			goto err_clk_disable;
	}

	ret = request_irq(dart->irq, apple_dart_irq, IRQF_SHARED,
			  "apple-dart fault handler", dart);
	if (ret)
		goto err_clk_disable;

	platform_set_drvdata(pdev, dart);

	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL, "apple-dart.%s",
				     dev_name(&pdev->dev));
	if (ret)
		goto err_free_irq;

	ret = iommu_device_register(&dart->iommu, &apple_dart_iommu_ops, dev);
	if (ret)
		goto err_sysfs_remove;

	pm_runtime_put(dev);

	dev_info(
		&pdev->dev,
		"DART [pagesize %x, %d streams, bypass support: %d, bypass forced: %d, locked: %d, AS %d -> %d] initialized\n",
		dart->pgsize, dart->num_streams, dart->supports_bypass,
		dart->pgsize > PAGE_SIZE, dart->locked, dart->ias, dart->oas);
	return 0;

err_sysfs_remove:
	iommu_device_sysfs_remove(&dart->iommu);
err_free_irq:
	free_irq(dart->irq, dart);
err_clk_disable:
	pm_runtime_put(dev);
	clk_bulk_disable_unprepare(dart->num_clks, dart->clks);

	return ret;
}

static void apple_dart_remove(struct platform_device *pdev)
{
	struct apple_dart *dart = platform_get_drvdata(pdev);

	if (!dart->locked)
		apple_dart_hw_reset(dart);

	free_irq(dart->irq, dart);

	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);

	clk_bulk_disable_unprepare(dart->num_clks, dart->clks);
}

static const struct apple_dart_hw apple_dart_hw_t8103 = {
	.type = DART_T8020,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.oas = 36,
	.fmt = APPLE_DART,
	.max_sid_count = 16,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,

	.ttbr = DART_T8020_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t8103_usb4 = {
	.type = DART_T8020,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.oas = 36,
	.fmt = APPLE_DART,
	.max_sid_count = 64,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = 0,

	.ttbr = DART_T8020_USB4_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t6000 = {
	.type = DART_T6000,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.oas = 42,
	.fmt = APPLE_DART2,
	.max_sid_count = 16,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,

	.ttbr = DART_T8020_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t8110 = {
	.type = DART_T8110,
	.irq_handler = apple_dart_t8110_irq,
	.invalidate_tlb = apple_dart_t8110_hw_invalidate_tlb,
	.fmt = APPLE_DART2,
	.max_sid_count = 256,

	.enable_streams = DART_T8110_ENABLE_STREAMS,
	.lock = DART_T8110_PROTECT,
	.lock_bit = DART_T8110_PROTECT_TTBR_TCR,

	.error = DART_T8110_ERROR,

	.tcr = DART_T8110_TCR,
	.tcr_enabled = DART_T8110_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART,
	.tcr_4level = DART_T8110_TCR_FOUR_LEVEL,

	.ttbr = DART_T8110_TTBR,
	.ttbr_valid = DART_T8110_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8110_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8110_TTBR_SHIFT,
	.ttbr_count = 1,
};

static __maybe_unused int apple_dart_suspend(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned int sid, idx;

	for (sid = 0; sid < dart->num_streams; sid++) {
		dart->save_tcr[sid] = readl(dart->regs + DART_TCR(dart, sid));
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			dart->save_ttbr[sid][idx] =
				readl(dart->regs + DART_TTBR(dart, sid, idx));
	}

	return 0;
}

static __maybe_unused int apple_dart_resume(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned int sid, idx;
	int ret;

	/* Locked DARTs can't be restored, and they should not need it */
	if (dart->locked)
		return 0;

	ret = apple_dart_hw_reset(dart);
	if (ret) {
		dev_err(dev, "Failed to reset DART on resume\n");
		return ret;
	}

	for (sid = 0; sid < dart->num_streams; sid++) {
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			writel(dart->save_ttbr[sid][idx],
			       dart->regs + DART_TTBR(dart, sid, idx));
		writel(dart->save_tcr[sid], dart->regs + DART_TCR(dart, sid));
	}

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(apple_dart_pm_ops, apple_dart_suspend, apple_dart_resume, NULL);

static const struct of_device_id apple_dart_of_match[] = {
	{ .compatible = "apple,t8103-dart", .data = &apple_dart_hw_t8103 },
	{ .compatible = "apple,t8103-usb4-dart", .data = &apple_dart_hw_t8103_usb4 },
	{ .compatible = "apple,t8110-dart", .data = &apple_dart_hw_t8110 },
	{ .compatible = "apple,t6000-dart", .data = &apple_dart_hw_t6000 },
	{},
};
MODULE_DEVICE_TABLE(of, apple_dart_of_match);

static struct platform_driver apple_dart_driver = {
	.driver	= {
		.name			= "apple-dart",
		.of_match_table		= apple_dart_of_match,
		.suppress_bind_attrs    = true,
		.pm			= pm_ptr(&apple_dart_pm_ops),
	},
	.probe	= apple_dart_probe,
	.remove = apple_dart_remove,
};

module_platform_driver(apple_dart_driver);

MODULE_DESCRIPTION("IOMMU API for Apple's DART");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("GPL v2");
