/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
#ifndef _KMPP_MPIBUF_H
#define _KMPP_MPIBUF_H

struct device;

void kmpp_mpibuf_register(void);
void kmpp_mpibuf_set_dev(struct device *dev);
int kmpp_alloc_dmabuf_fd(size_t size);
void kmpp_import_cache_flush(void);

#endif
