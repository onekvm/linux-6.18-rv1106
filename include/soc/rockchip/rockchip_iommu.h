/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROCKCHIP_IOMMU_H
#define _ROCKCHIP_IOMMU_H

struct device;

static inline int rockchip_iommu_enable(struct device *dev)
{
	return 0;
}

static inline int rockchip_iommu_disable(struct device *dev)
{
	return 0;
}

#endif
