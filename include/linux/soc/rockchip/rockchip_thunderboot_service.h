/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROCKCHIP_THUNDERBOOT_SERVICE_H
#define _ROCKCHIP_THUNDERBOOT_SERVICE_H

#include <linux/rk-isp2-config.h>

struct rk_tb_client {
	void *data;
	void (*cb)(void *data);
};

static inline bool rk_tb_mcu_is_done(void)
{
	return true;
}

static inline int rk_tb_client_register_cb(struct rk_tb_client *client)
{
	return 0;
}

#endif
