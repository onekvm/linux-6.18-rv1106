/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
#ifndef _KMPP_COMPAT_H
#define _KMPP_COMPAT_H

#include <linux/of.h>
#include <linux/platform_device.h>

#ifndef PDE_DATA
#define PDE_DATA(inode) pde_data(inode)
#endif

#endif
