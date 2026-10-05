/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROCKCHIP_ROCKIT_H
#define _ROCKCHIP_ROCKIT_H

#include <linux/dma-buf.h>
#include <linux/types.h>

#define ROCKIT_BUF_NUM_MAX		8
#define ROCKIT_CIF_NUM_MAX		8
#define ROCKIT_STREAM_NUM_MAX		4

#define ROCKIT_BUF_QUE			0
#define ROCKIT_MPIBUF_DONE		1

struct rkcif_stream_cfg {
	void *node;
	int buff_id[ROCKIT_BUF_NUM_MAX];
	void *rkcif_buff[ROCKIT_BUF_NUM_MAX];
	bool is_discard;
};

struct rkcif_dev_cfg {
	const char *cif_name;
	void *cif_dev;
	struct rkcif_stream_cfg rkcif_stream_cfg[ROCKIT_STREAM_NUM_MAX];
};

struct rockit_rkcif_cfg {
	int cif_num;
	int nick_id;
	int mpi_id;
	int is_alloc;
	bool is_empty;
	const char *cur_name;
	void *node;
	void *mpibuf;
	struct dma_buf *buf;
	int (*rkcif_rockit_mpibuf_done)(struct rockit_rkcif_cfg *cfg);
	struct rkcif_dev_cfg rkcif_dev_cfg[ROCKIT_CIF_NUM_MAX];
	struct {
		u64 u64PTS;
		u32 u32TimeRef;
		u32 u32Height;
		u32 u32Width;
		u32 enPixelFormat;
		u32 u32VirWidth;
		u32 u32VirHeight;
	} frame;
};

#endif
