// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * CMA/dma-buf stand-in for rockit.ko mpi_buf ops so /dev/vcodec can encode
 * without librockit on Linux 6.18.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/iosys-map.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "kmpp_mpibuf.h"
#include "rk_type.h"
#include "rk_export_func.h"

extern void register_vmpibuf_func_to_pp(struct vcodec_mpibuf_fn *mpibuf_fn);

struct mpi_buf_pool;

struct mpi_buf {
	struct kref ref;
	struct dma_buf *dmabuf;
	void *vaddr;
	dma_addr_t paddr;
	size_t size;
	struct mpp_frame_infos info;
	bool imported;
	bool vmapped;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct mpi_buf_pool *pool;
};

struct mpi_queue {
	spinlock_t lock;
	struct list_head list;
	int max;
	int count;
};

struct mpi_qitem {
	struct list_head node;
	struct mpi_buf *buf;
};

struct mpi_buf_pool {
	size_t buf_size;
	unsigned int num_bufs;
	unsigned int free;
	unsigned int outstanding;
	bool dead;
	spinlock_t lock;
	struct mpi_buf **bufs;
};

struct kmpp_cma {
	struct device *dev;
	void *vaddr;
	dma_addr_t dma;
	size_t size;
	struct sg_table sgt_base;
};

struct kmpp_attach {
	struct sg_table sgt;
	enum dma_data_direction dma_dir;
};

static struct device *kmpp_dma_dev;
static char kmpp_dummy_mpi_dev;

#define KMPP_IMPORT_CACHE 8

struct kmpp_import_slot {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t paddr;
};

static DEFINE_MUTEX(kmpp_import_mutex);
static struct kmpp_import_slot kmpp_import_cache[KMPP_IMPORT_CACHE];

static int kmpp_import_cache_paddr(struct dma_buf *dmabuf, struct device *dev,
				   dma_addr_t *paddr)
{
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	int i;
	int empty = -1;
	int ret = 0;

	mutex_lock(&kmpp_import_mutex);
	for (i = 0; i < KMPP_IMPORT_CACHE; i++) {
		if (kmpp_import_cache[i].dmabuf == dmabuf) {
			*paddr = kmpp_import_cache[i].paddr;
			mutex_unlock(&kmpp_import_mutex);
			return 0;
		}
		if (!kmpp_import_cache[i].dmabuf && empty < 0)
			empty = i;
	}
	mutex_unlock(&kmpp_import_mutex);

	if (empty < 0)
		return -ENOSPC;

	attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(attach))
		return PTR_ERR(attach);
	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		dma_buf_detach(dmabuf, attach);
		return PTR_ERR(sgt);
	}

	mutex_lock(&kmpp_import_mutex);
	empty = -1;
	for (i = 0; i < KMPP_IMPORT_CACHE; i++) {
		if (kmpp_import_cache[i].dmabuf == dmabuf) {
			*paddr = kmpp_import_cache[i].paddr;
			mutex_unlock(&kmpp_import_mutex);
			dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
			dma_buf_detach(dmabuf, attach);
			return 0;
		}
		if (!kmpp_import_cache[i].dmabuf && empty < 0)
			empty = i;
	}
	if (empty < 0) {
		ret = -ENOSPC;
		goto out_unlock;
	}
	get_dma_buf(dmabuf);
	kmpp_import_cache[empty].dmabuf = dmabuf;
	kmpp_import_cache[empty].attach = attach;
	kmpp_import_cache[empty].sgt = sgt;
	kmpp_import_cache[empty].paddr = sg_dma_address(sgt->sgl);
	*paddr = kmpp_import_cache[empty].paddr;
	attach = NULL;
out_unlock:
	mutex_unlock(&kmpp_import_mutex);
	if (attach) {
		dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
		dma_buf_detach(dmabuf, attach);
	}
	return ret;
}

void kmpp_import_cache_flush(void)
{
	int i;

	mutex_lock(&kmpp_import_mutex);
	for (i = 0; i < KMPP_IMPORT_CACHE; i++) {
		struct kmpp_import_slot *slot = &kmpp_import_cache[i];

		if (!slot->dmabuf)
			continue;
		if (slot->sgt && slot->attach)
			dma_buf_unmap_attachment_unlocked(slot->attach, slot->sgt,
							    DMA_BIDIRECTIONAL);
		if (slot->attach)
			dma_buf_detach(slot->dmabuf, slot->attach);
		dma_buf_put(slot->dmabuf);
		memset(slot, 0, sizeof(*slot));
	}
	mutex_unlock(&kmpp_import_mutex);
}

static struct device *kmpp_dma_device(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	if (kmpp_dma_dev)
		return kmpp_dma_dev;

	np = of_find_compatible_node(NULL, NULL, "rockchip,rkv-encoder-rv1106");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	kmpp_dma_dev = &pdev->dev;
	return kmpp_dma_dev;
}

void kmpp_mpibuf_set_dev(struct device *dev)
{
	if (dev && !kmpp_dma_dev)
		kmpp_dma_dev = get_device(dev);
}

static int kmpp_dmabuf_attach(struct dma_buf *dbuf,
			      struct dma_buf_attachment *attach)
{
	struct kmpp_cma *cma = dbuf->priv;
	struct kmpp_attach *at;
	struct scatterlist *rd, *wr;
	unsigned int i;
	int ret;

	at = kzalloc(sizeof(*at), GFP_KERNEL);
	if (!at)
		return -ENOMEM;

	ret = sg_alloc_table(&at->sgt, cma->sgt_base.orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(at);
		return ret;
	}

	rd = cma->sgt_base.sgl;
	wr = at->sgt.sgl;
	for (i = 0; i < at->sgt.orig_nents; i++) {
		sg_set_page(wr, sg_page(rd), rd->length, rd->offset);
		rd = sg_next(rd);
		wr = sg_next(wr);
	}
	at->dma_dir = DMA_NONE;
	attach->priv = at;
	return 0;
}

static void kmpp_dmabuf_detach(struct dma_buf *dbuf,
			       struct dma_buf_attachment *attach)
{
	struct kmpp_attach *at = attach->priv;

	if (!at)
		return;
	if (at->dma_dir != DMA_NONE)
		dma_unmap_sgtable(attach->dev, &at->sgt, at->dma_dir,
				  DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(&at->sgt);
	kfree(at);
	attach->priv = NULL;
}

static struct sg_table *kmpp_dmabuf_map(struct dma_buf_attachment *attach,
					 enum dma_data_direction dir)
{
	struct kmpp_attach *at = attach->priv;
	int ret;

	if (at->dma_dir == dir)
		return &at->sgt;

	if (at->dma_dir != DMA_NONE) {
		dma_unmap_sgtable(attach->dev, &at->sgt, at->dma_dir,
				  DMA_ATTR_SKIP_CPU_SYNC);
		at->dma_dir = DMA_NONE;
	}

	ret = dma_map_sgtable(attach->dev, &at->sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		return ERR_PTR(ret);
	at->dma_dir = dir;
	return &at->sgt;
}

static void kmpp_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
}

static int kmpp_dmabuf_mmap(struct dma_buf *dbuf, struct vm_area_struct *vma)
{
	struct kmpp_cma *cma = dbuf->priv;

	return dma_mmap_attrs(cma->dev, vma, cma->vaddr, cma->dma, cma->size, 0);
}

static int kmpp_dmabuf_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct kmpp_cma *cma = dbuf->priv;

	iosys_map_set_vaddr(map, cma->vaddr);
	return 0;
}

static void kmpp_dmabuf_release(struct dma_buf *dbuf)
{
	struct kmpp_cma *cma = dbuf->priv;

	sg_free_table(&cma->sgt_base);
	dma_free_coherent(cma->dev, cma->size, cma->vaddr, cma->dma);
	kfree(cma);
}

static const struct dma_buf_ops kmpp_dmabuf_ops = {
	.attach = kmpp_dmabuf_attach,
	.detach = kmpp_dmabuf_detach,
	.map_dma_buf = kmpp_dmabuf_map,
	.unmap_dma_buf = kmpp_dmabuf_unmap,
	.mmap = kmpp_dmabuf_mmap,
	.vmap = kmpp_dmabuf_vmap,
	.release = kmpp_dmabuf_release,
};

static struct dma_buf *kmpp_export_cma(struct device *dev, size_t size,
					void **vaddr, dma_addr_t *paddr)
{
	struct kmpp_cma *cma;
	struct dma_buf *dbuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int ret;

	cma = kzalloc(sizeof(*cma), GFP_KERNEL);
	if (!cma)
		return ERR_PTR(-ENOMEM);

	cma->dev = dev;
	cma->size = PAGE_ALIGN(size);
	cma->vaddr = dma_alloc_coherent(dev, cma->size, &cma->dma, GFP_KERNEL);
	if (!cma->vaddr) {
		kfree(cma);
		return ERR_PTR(-ENOMEM);
	}

	ret = dma_get_sgtable(dev, &cma->sgt_base, cma->vaddr, cma->dma, cma->size);
	if (ret) {
		dma_free_coherent(dev, cma->size, cma->vaddr, cma->dma);
		kfree(cma);
		return ERR_PTR(ret);
	}

	exp_info.ops = &kmpp_dmabuf_ops;
	exp_info.size = cma->size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = cma;
	exp_info.exp_name = "kmpp";
	dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(dbuf)) {
		sg_free_table(&cma->sgt_base);
		dma_free_coherent(dev, cma->size, cma->vaddr, cma->dma);
		kfree(cma);
		return dbuf;
	}

	*vaddr = cma->vaddr;
	*paddr = cma->dma;
	return dbuf;
}

int kmpp_alloc_dmabuf_fd(size_t size)
{
	struct device *dev = kmpp_dma_device();
	struct dma_buf *dbuf;
	void *vaddr;
	dma_addr_t paddr;
	int fd;

	if (!dev || !size)
		return -EINVAL;

	dbuf = kmpp_export_cma(dev, size, &vaddr, &paddr);
	if (IS_ERR(dbuf))
		return PTR_ERR(dbuf);

	fd = dma_buf_fd(dbuf, O_CLOEXEC | O_RDWR);
	if (fd < 0) {
		dma_buf_put(dbuf);
		return fd;
	}
	/* dma_buf_fd() fd_installs dmabuf->file; do not put the export ref. */
	return fd;
}

static void mpi_buf_free_storage(struct mpi_buf *buf)
{
	if (buf->vmapped && buf->dmabuf) {
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(buf->vaddr);

		dma_buf_vunmap_unlocked(buf->dmabuf, &map);
		buf->vmapped = false;
	}
	if (buf->sgt && buf->attach)
		dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
						    DMA_BIDIRECTIONAL);
	if (buf->attach && buf->dmabuf)
		dma_buf_detach(buf->dmabuf, buf->attach);
	if (buf->dmabuf)
		dma_buf_put(buf->dmabuf);
	kfree(buf);
}

static void buf_pool_free(struct mpi_buf_pool *pool)
{
	kfree(pool->bufs);
	kfree(pool);
}

/*
 * rockit.ko strm_pool: request_buf takes a slot; last buf_unref puts it
 * back. Without this, get_free_num hits 0 after 3 encodes and
 * enc_chan_process_single_chan skips forever (browser 0 FPS).
 */
static bool buf_pool_recycle(struct mpi_buf *buf)
{
	struct mpi_buf_pool *pool = buf->pool;
	unsigned long flags;
	unsigned int i;
	bool drop_pool = false;

	if (!pool)
		return false;

	spin_lock_irqsave(&pool->lock, flags);
	if (pool->outstanding)
		pool->outstanding--;
	if (pool->dead) {
		buf->pool = NULL;
		drop_pool = (pool->outstanding == 0);
		spin_unlock_irqrestore(&pool->lock, flags);
		mpi_buf_free_storage(buf);
		if (drop_pool)
			buf_pool_free(pool);
		return true;
	}
	for (i = 0; i < pool->num_bufs; i++) {
		if (!pool->bufs[i]) {
			pool->bufs[i] = buf;
			pool->free++;
			kref_init(&buf->ref);
			spin_unlock_irqrestore(&pool->lock, flags);
			pr_info_once("kmpp_mpibuf: strm_pool recycle\n");
			return true;
		}
	}
	buf->pool = NULL;
	drop_pool = (pool->outstanding == 0 && pool->dead);
	spin_unlock_irqrestore(&pool->lock, flags);
	pr_warn_once("kmpp_mpibuf: strm_pool recycle missed a slot\n");
	mpi_buf_free_storage(buf);
	if (drop_pool)
		buf_pool_free(pool);
	return true;
}

static void mpi_buf_release(struct kref *ref)
{
	struct mpi_buf *buf = container_of(ref, struct mpi_buf, ref);

	if (buf_pool_recycle(buf))
		return;
	mpi_buf_free_storage(buf);
}

static struct mpi_buf *buf_alloc(size_t size)
{
	struct device *dev = kmpp_dma_device();
	struct mpi_buf *buf;
	struct dma_buf *dbuf;
	void *vaddr;
	dma_addr_t paddr;

	if (!dev || !size)
		return NULL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;

	dbuf = kmpp_export_cma(dev, size, &vaddr, &paddr);
	if (IS_ERR(dbuf)) {
		kfree(buf);
		return NULL;
	}

	kref_init(&buf->ref);
	buf->dmabuf = dbuf;
	buf->vaddr = vaddr;
	buf->paddr = paddr;
	buf->size = PAGE_ALIGN(size);
	return buf;
}

static void *buf_map(struct mpi_buf *buf)
{
	struct iosys_map map;
	int ret;

	if (!buf)
		return NULL;
	if (buf->vaddr)
		return buf->vaddr;
	if (!buf->dmabuf)
		return NULL;

	ret = dma_buf_vmap_unlocked(buf->dmabuf, &map);
	if (ret)
		return NULL;
	buf->vaddr = map.vaddr;
	buf->vmapped = true;
	return buf->vaddr;
}

static int buf_unmap(struct mpi_buf *buf)
{
	return 0;
}

static void buf_ref(struct mpi_buf *buf)
{
	if (buf)
		kref_get(&buf->ref);
}

static void buf_unref(struct mpi_buf *buf)
{
	if (buf)
		kref_put(&buf->ref, mpi_buf_release);
}

static struct dma_buf *buf_get_dmabuf(struct mpi_buf *buf)
{
	return buf ? buf->dmabuf : NULL;
}

static int buf_get_paddr(struct mpi_buf *buf)
{
	return buf ? (int)buf->paddr : 0;
}

static struct mpi_queue *buf_queue_create(int cnt)
{
	struct mpi_queue *q;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return NULL;
	spin_lock_init(&q->lock);
	INIT_LIST_HEAD(&q->list);
	q->max = cnt > 0 ? cnt : 8;
	return q;
}

static int buf_queue_destroy(struct mpi_queue *queue)
{
	struct mpi_qitem *item, *tmp;

	if (!queue)
		return 0;

	spin_lock(&queue->lock);
	list_for_each_entry_safe(item, tmp, &queue->list, node) {
		list_del(&item->node);
		buf_unref(item->buf);
		kfree(item);
	}
	spin_unlock(&queue->lock);
	kfree(queue);
	return 0;
}

static int buf_queue_push(struct mpi_queue *queue, struct mpi_buf *buf)
{
	struct mpi_qitem *item;
	unsigned long flags;

	if (!queue || !buf)
		return -EINVAL;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;
	item->buf = buf;

	spin_lock_irqsave(&queue->lock, flags);
	while (queue->max > 0 && queue->count >= queue->max) {
		struct mpi_qitem *old =
			list_first_entry(&queue->list, struct mpi_qitem, node);

		list_del(&old->node);
		queue->count--;
		spin_unlock_irqrestore(&queue->lock, flags);
		buf_unref(old->buf);
		kfree(old);
		spin_lock_irqsave(&queue->lock, flags);
	}
	list_add_tail(&item->node, &queue->list);
	queue->count++;
	spin_unlock_irqrestore(&queue->lock, flags);
	return 1;
}

static struct mpi_buf *buf_queue_pop(struct mpi_queue *queue)
{
	struct mpi_qitem *item;
	struct mpi_buf *buf = NULL;
	unsigned long flags;

	if (!queue)
		return NULL;

	spin_lock_irqsave(&queue->lock, flags);
	item = list_first_entry_or_null(&queue->list, struct mpi_qitem, node);
	if (item) {
		list_del(&item->node);
		queue->count--;
		buf = item->buf;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	kfree(item);
	return buf;
}

static struct mpi_buf *dma_buf_import(struct dma_buf *dmabuf,
				       struct mpp_frame_infos *info,
				       int chan_id)
{
	struct mpi_buf *buf;
	struct device *dev = kmpp_dma_device();

	if (!dmabuf)
		return NULL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;

	get_dma_buf(dmabuf);
	kref_init(&buf->ref);
	buf->dmabuf = dmabuf;
	buf->imported = true;
	buf->size = dmabuf->size;
	if (info)
		buf->info = *info;
	buf->info.mpi_buf_id = chan_id;

	if (dev) {
		if (kmpp_import_cache_paddr(dmabuf, dev, &buf->paddr)) {
			pr_err("kmpp_mpibuf: import cache attach failed\n");
			dma_buf_put(dmabuf);
			kfree(buf);
			return NULL;
		}
	} else if (info && info->phy_addr) {
		buf->paddr = info->phy_addr;
	} else {
		dma_buf_put(dmabuf);
		kfree(buf);
		return NULL;
	}

	return buf;
}

static int get_buf_frm_info(struct mpi_buf *buf, struct mpp_frame_infos *info,
			     RK_S32 chan_id)
{
	if (!buf || !info)
		return -EINVAL;
	*info = buf->info;
	if (!info->phy_addr)
		info->phy_addr = (RK_U32)buf->paddr;
	(void)chan_id;
	return 0;
}

static struct mpi_buf_pool *buf_pool_create(size_t buf_size, unsigned int num_bufs)
{
	struct mpi_buf_pool *pool;
	unsigned int i;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;
	pool->bufs = kcalloc(num_bufs, sizeof(*pool->bufs), GFP_KERNEL);
	if (!pool->bufs) {
		kfree(pool);
		return NULL;
	}
	spin_lock_init(&pool->lock);
	pool->buf_size = buf_size;
	pool->num_bufs = num_bufs;
	for (i = 0; i < num_bufs; i++) {
		pool->bufs[i] = buf_alloc(buf_size);
		if (!pool->bufs[i]) {
			while (i--) {
				pool->bufs[i]->pool = NULL;
				buf_unref(pool->bufs[i]);
			}
			kfree(pool->bufs);
			kfree(pool);
			return NULL;
		}
		pool->bufs[i]->pool = pool;
		pool->free++;
	}
	return pool;
}

static int buf_pool_destroy(struct mpi_buf_pool *pool)
{
	unsigned int i;
	unsigned long flags;
	bool drop_pool;

	if (!pool)
		return 0;

	spin_lock_irqsave(&pool->lock, flags);
	pool->dead = true;
	for (i = 0; i < pool->num_bufs; i++) {
		struct mpi_buf *buf = pool->bufs[i];

		if (!buf)
			continue;
		pool->bufs[i] = NULL;
		if (pool->free)
			pool->free--;
		buf->pool = NULL;
		spin_unlock_irqrestore(&pool->lock, flags);
		buf_unref(buf);
		spin_lock_irqsave(&pool->lock, flags);
	}
	drop_pool = (pool->outstanding == 0);
	spin_unlock_irqrestore(&pool->lock, flags);
	if (drop_pool)
		buf_pool_free(pool);
	return 0;
}

static struct mpi_buf *buf_pool_request_buf(struct mpi_buf_pool *pool)
{
	unsigned int i;
	unsigned long flags;
	struct mpi_buf *buf = NULL;

	if (!pool)
		return NULL;

	spin_lock_irqsave(&pool->lock, flags);
	if (pool->dead) {
		spin_unlock_irqrestore(&pool->lock, flags);
		return NULL;
	}
	for (i = 0; i < pool->num_bufs; i++) {
		if (pool->bufs[i]) {
			buf = pool->bufs[i];
			pool->bufs[i] = NULL;
			pool->free--;
			pool->outstanding++;
			break;
		}
	}
	spin_unlock_irqrestore(&pool->lock, flags);
	return buf;
}

static int buf_pool_get_free_num(struct mpi_buf_pool *pool)
{
	/*
	 * strm_pool is created lazily in mpp_enc_alloc_output(). Returning 0
	 * for a NULL pool made enc_chan_process_single_chan skip every frame
	 * (CIF DQBUF / IN_FRM_RDY succeeded, VEPU IRQ stayed 0).
	 */
	return pool ? (int)pool->free : 1;
}

static struct vcodec_mpibuf_fn kmpp_mpibuf_ops = {
	.buf_alloc = buf_alloc,
	.buf_map = buf_map,
	.buf_unmap = buf_unmap,
	.buf_ref = buf_ref,
	.buf_unref = buf_unref,
	.buf_get_dmabuf = buf_get_dmabuf,
	.buf_get_paddr = buf_get_paddr,
	.buf_queue_create = buf_queue_create,
	.buf_queue_destroy = buf_queue_destroy,
	.buf_queue_push = buf_queue_push,
	.buf_queue_pop = buf_queue_pop,
	.dma_buf_import = dma_buf_import,
	.get_buf_frm_info = get_buf_frm_info,
	.buf_pool_create = buf_pool_create,
	.buf_pool_destroy = buf_pool_destroy,
	.buf_pool_request_buf = buf_pool_request_buf,
	.buf_pool_get_free_num = buf_pool_get_free_num,
};

static struct mpi_dev *kmpp_create_dev(const char *name,
					struct vcodec_set_dev_fn *dev_fn)
{
	(void)name;
	(void)dev_fn;
	return (struct mpi_dev *)&kmpp_dummy_mpi_dev;
}

static int kmpp_destroy_dev(struct mpi_dev *dev)
{
	(void)dev;
	return 0;
}

static struct vcodec_mpidev_fn kmpp_mpidev_ops = {
	.create_dev = kmpp_create_dev,
	.destory_dev = kmpp_destroy_dev,
};

void kmpp_mpibuf_register(void)
{
	vmpi_register_fn2vcocdec(&kmpp_mpidev_ops, &kmpp_mpibuf_ops);
	register_vmpibuf_func_to_pp(&kmpp_mpibuf_ops);
}
