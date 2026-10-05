// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip Crypto Engine v3 — AES-GCM + OneKVM SRTP TX offload.
 *
 * Register map and LLI programming come from the vendor 5.10 rkcrypto
 * driver (rk_crypto_v2_reg.h / rk_crypto_v3_skcipher.c). This is a 6.18
 * port that only implements AES-GCM encrypt for SRTP, not the full
 * SM4/RSA/hash stack.
 */

#include <crypto/aes.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define CE_CLK_CTL			0x0000
#define CE_AUTO_CLKGATE_EN		BIT(0)
#define CE_RST_CTL			0x0004
#define CE_SW_CC_RESET			BIT(0)
#define CE_DMA_INT_EN			0x0008
#define CE_DMA_INT_ST			0x000c
#define CE_DST_ITEM_DONE_INT_ST		BIT(1)
#define CE_LIST_DONE_INT_ST		BIT(0)
#define CE_DMA_ERR_INT_ST		(BIT(3) | BIT(4) | BIT(5) | BIT(6))
#define CE_DMA_CTL			0x0010
#define CE_DMA_START			BIT(0)
#define CE_DMA_LLI_ADDR			0x0014
#define CE_FIFO_CTL			0x0040
#define CE_BC_CTL			0x0044
#define CE_BC_AES			(0x00 << 8)
#define CE_BC_GCM			(0x08 << 4)
#define CE_BC_128_KEY			(0x00 << 2)
#define CE_BC_192_KEY			(0x01 << 2)
#define CE_BC_256_KEY			(0x02 << 2)
#define CE_BC_ENABLE			BIT(0)
#define CE_WRITE_MASK_ALL		(0xffffu << 16)
#define CE_CH0_IV_0			0x0100
#define CE_CH0_KEY_0			0x0180
#define CE_CH0_PC_LEN_0			0x0280
#define CE_CH0_AAD_LEN_0		0x02c0
#define CE_CH0_IV_LEN_0			0x0300
#define CE_CH0_TAG_0			0x0320
#define CE_TAG_VALID			0x03e0
#define CE_CH0_TAG_VALID		BIT(0)
#define CE_AES_VERSION			0x0680
#define CE_GCM_FLAG			BIT(8)

#define LLI_DMA_CTRL_LAST		BIT(0)
#define LLI_DMA_CTRL_DST_DONE		BIT(9)
#define LLI_USER_CIPHER_START		BIT(0)
#define LLI_USER_STRING_START		BIT(1)
#define LLI_USER_STRING_LAST		BIT(2)
#define LLI_USER_STRING_AAD		BIT(3)

#define ONEKVM_OFFLOAD_NAME		"onekvm-crypto-offload"
#define ONEKVM_OFFLOAD_ABI		1
#define ONEKVM_OFFLOAD_IOCTL_TYPE	0xb7
#define ONEKVM_OFFLOAD_GCM_IV		12
#define ONEKVM_OFFLOAD_GCM_TAG		16
#define ONEKVM_OFFLOAD_MAX_INPUT	(20 * 1024)
#define ONEKVM_OFFLOAD_MAX_AAD		256
#define ONEKVM_OFFLOAD_MAX_BATCH	128
#define ONEKVM_OFFLOAD_POLL_US		20000

struct crypto_lli_desc {
	u32 src_addr;
	u32 src_len;
	u32 dst_addr;
	u32 dst_len;
	u32 user_define;
	u32 reserve;
	u32 dma_ctrl;
	u32 next_addr;
};

struct onekvm_offload_key_config {
	__u32 version;
	__u32 key_len;
	__u32 auth_size;
	__u32 reserved;
	__u8 key[32];
};

struct onekvm_offload_encrypt_request {
	__u32 version;
	__u32 flags;
	__u32 aad_len;
	__u32 data_len;
	__aligned_u64 aad_ptr;
	__aligned_u64 src_ptr;
	__aligned_u64 dst_ptr;
	__u8 iv[ONEKVM_OFFLOAD_GCM_IV];
	__u8 reserved[4];
};

struct onekvm_offload_batch_request {
	__u32 version;
	__u32 flags;
	__u32 count;
	__u32 completed;
	__aligned_u64 requests_ptr;
};

static_assert(sizeof(struct onekvm_offload_key_config) == 48);
static_assert(sizeof(struct onekvm_offload_encrypt_request) == 56);
static_assert(sizeof(struct onekvm_offload_batch_request) == 24);

#define ONEKVM_OFFLOAD_SET_KEY \
	_IOW(ONEKVM_OFFLOAD_IOCTL_TYPE, 0x01, struct onekvm_offload_key_config)
#define ONEKVM_OFFLOAD_ENCRYPT \
	_IOWR(ONEKVM_OFFLOAD_IOCTL_TYPE, 0x02, struct onekvm_offload_encrypt_request)
#define ONEKVM_OFFLOAD_ENCRYPT_BATCH \
	_IOWR(ONEKVM_OFFLOAD_IOCTL_TYPE, 0x03, struct onekvm_offload_batch_request)

struct rk_ce {
	struct device *dev;
	void __iomem *reg;
	struct clk_bulk_data clks[4];
	int nclks;
	struct reset_control *rst;
	struct mutex lock;
	struct crypto_lli_desc *lli;
	dma_addr_t lli_dma;
	u8 *aad;
	dma_addr_t aad_dma;
	u8 *src;
	dma_addr_t src_dma;
	u8 *dst;
	dma_addr_t dst_dma;
};

struct offload_ctx {
	u8 key[32];
	unsigned int key_len;
};

static struct rk_ce *g_ce;
static atomic64_t offload_tx_packets = ATOMIC64_INIT(0);
static atomic64_t offload_tx_bytes = ATOMIC64_INIT(0);
static atomic64_t offload_tx_errors = ATOMIC64_INIT(0);
static atomic64_t offload_batch_calls = ATOMIC64_INIT(0);
static atomic64_t offload_crypto_ns = ATOMIC64_INIT(0);

static inline u32 ce_read(struct rk_ce *ce, u32 off)
{
	return readl_relaxed(ce->reg + off);
}

static inline void ce_write(struct rk_ce *ce, u32 off, u32 val)
{
	writel_relaxed(val, ce->reg + off);
}

static u32 byte2word_be(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void word2byte_be(u32 word, u8 *p)
{
	p[0] = word >> 24;
	p[1] = word >> 16;
	p[2] = word >> 8;
	p[3] = word;
}

static void ce_write_be(struct rk_ce *ce, u32 base, const u8 *data, u32 bytes)
{
	u32 i;
	u8 pad[4] = { 0 };

	for (i = 0; i < bytes / 4; i++, base += 4)
		ce_write(ce, base, byte2word_be(data + i * 4));
	if (bytes % 4) {
		memcpy(pad, data + (bytes / 4) * 4, bytes % 4);
		ce_write(ce, base, byte2word_be(pad));
	}
}

static void ce_read_be(struct rk_ce *ce, u32 base, u8 *data, u32 bytes)
{
	u32 i;

	for (i = 0; i < bytes / 4; i++, base += 4)
		word2byte_be(ce_read(ce, base), data + i * 4);
}

static void ce_reset(struct rk_ce *ce)
{
	u32 st;
	int i;

	ce_write(ce, CE_DMA_INT_EN, 0);
	ce_write(ce, CE_RST_CTL, CE_SW_CC_RESET | (CE_SW_CC_RESET << 16));
	for (i = 0; i < 1000; i++) {
		if (!ce_read(ce, CE_RST_CTL))
			break;
		cpu_relax();
	}
	ce_write(ce, CE_BC_CTL, CE_WRITE_MASK_ALL);
	st = ce_read(ce, CE_DMA_INT_ST);
	if (st)
		ce_write(ce, CE_DMA_INT_ST, st);
}

static int ce_poll_done(struct rk_ce *ce)
{
	ktime_t deadline = ktime_add_us(ktime_get(), ONEKVM_OFFLOAD_POLL_US);
	u32 st;

	do {
		st = ce_read(ce, CE_DMA_INT_ST);
		if (st & CE_DMA_ERR_INT_ST) {
			ce_write(ce, CE_DMA_INT_ST, st);
			dev_err_ratelimited(ce->dev, "CE DMA err 0x%x\n", st);
			return -EIO;
		}
		if (st & (CE_DST_ITEM_DONE_INT_ST | CE_LIST_DONE_INT_ST)) {
			ce_write(ce, CE_DMA_INT_ST, st);
			return 0;
		}
		cpu_relax();
	} while (ktime_before(ktime_get(), deadline));
	return -ETIMEDOUT;
}

static int ce_gcm_encrypt(struct rk_ce *ce, const u8 *key, unsigned int key_len,
			  const u8 *iv, const u8 *aad, u32 aad_len,
			  const u8 *src, u32 data_len, u8 *dst, u8 *tag)
{
	struct crypto_lli_desc *lli_aad = &ce->lli[0];
	struct crypto_lli_desc *lli_data = &ce->lli[1];
	u32 mode = CE_BC_AES | CE_BC_GCM | CE_BC_ENABLE;
	u32 tag_st = 0;
	int i, error;
	dma_addr_t start;

	if (!data_len || data_len > ONEKVM_OFFLOAD_MAX_INPUT ||
	    aad_len > ONEKVM_OFFLOAD_MAX_AAD)
		return -EINVAL;
	if (key_len == AES_KEYSIZE_128)
		mode |= CE_BC_128_KEY;
	else if (key_len == AES_KEYSIZE_192)
		mode |= CE_BC_192_KEY;
	else if (key_len == AES_KEYSIZE_256)
		mode |= CE_BC_256_KEY;
	else
		return -EINVAL;

	ce_reset(ce);
	ce_write_be(ce, CE_CH0_KEY_0, key, key_len);
	ce_write_be(ce, CE_CH0_IV_0, iv, ONEKVM_OFFLOAD_GCM_IV);
	ce_write(ce, CE_CH0_IV_LEN_0, ONEKVM_OFFLOAD_GCM_IV);
	ce_write(ce, CE_CH0_PC_LEN_0, data_len);
	ce_write(ce, CE_CH0_PC_LEN_0 + 4, 0);
	ce_write(ce, CE_CH0_AAD_LEN_0, aad_len);
	ce_write(ce, CE_CH0_AAD_LEN_0 + 4, 0);

	if (aad_len)
		memcpy(ce->aad, aad, aad_len);
	memcpy(ce->src, src, data_len);

	memset(lli_aad, 0, sizeof(*lli_aad) * 2);
	if (aad_len) {
		lli_aad->src_addr = (u32)ce->aad_dma;
		lli_aad->src_len = aad_len;
		lli_aad->user_define = LLI_USER_CIPHER_START | LLI_USER_STRING_START |
				       LLI_USER_STRING_LAST | LLI_USER_STRING_AAD;
		lli_aad->next_addr = (u32)(ce->lli_dma + sizeof(*lli_aad));
		lli_data->user_define = LLI_USER_STRING_START | LLI_USER_STRING_LAST;
		start = ce->lli_dma;
	} else {
		lli_data->user_define = LLI_USER_CIPHER_START | LLI_USER_STRING_START |
					LLI_USER_STRING_LAST;
		start = ce->lli_dma + sizeof(*lli_aad);
	}
	lli_data->src_addr = (u32)ce->src_dma;
	lli_data->src_len = data_len;
	lli_data->dst_addr = (u32)ce->dst_dma;
	lli_data->dst_len = data_len;
	lli_data->dma_ctrl = LLI_DMA_CTRL_DST_DONE | LLI_DMA_CTRL_LAST;

	ce_write(ce, CE_FIFO_CTL, 0x00030003);
	ce_write(ce, CE_DMA_INT_EN, 0x7f);
	ce_write(ce, CE_BC_CTL, mode | CE_WRITE_MASK_ALL);
	dma_wmb();
	ce_write(ce, CE_DMA_LLI_ADDR, (u32)start);
	ce_write(ce, CE_DMA_CTL, CE_DMA_START | (CE_DMA_START << 16));

	error = ce_poll_done(ce);
	if (error)
		goto out;

	for (i = 0; i < 1000; i++) {
		tag_st = ce_read(ce, CE_TAG_VALID);
		if (tag_st & CE_CH0_TAG_VALID)
			break;
		cpu_relax();
	}
	if (!(tag_st & CE_CH0_TAG_VALID)) {
		error = -ETIMEDOUT;
		goto out;
	}
	ce_read_be(ce, CE_CH0_TAG_0, tag, ONEKVM_OFFLOAD_GCM_TAG);
	dma_rmb();
	memcpy(dst, ce->dst, data_len);
out:
	ce_write(ce, CE_BC_CTL, CE_WRITE_MASK_ALL);
	return error;
}

static int offload_validate(const struct onekvm_offload_encrypt_request *req)
{
	if (req->version != ONEKVM_OFFLOAD_ABI || req->flags)
		return -EINVAL;
	if (req->data_len > ONEKVM_OFFLOAD_MAX_INPUT ||
	    req->aad_len > ONEKVM_OFFLOAD_MAX_AAD ||
	    req->data_len + req->aad_len > ONEKVM_OFFLOAD_MAX_INPUT)
		return -EINVAL;
	if ((req->aad_len && !req->aad_ptr) || !req->src_ptr || !req->dst_ptr)
		return -EINVAL;
	if (!req->data_len)
		return -EINVAL;
	return 0;
}

static int offload_encrypt_one(struct rk_ce *ce, struct offload_ctx *ctx,
			       struct onekvm_offload_encrypt_request *req)
{
	u8 *aad = NULL, *src, *dst, tag[ONEKVM_OFFLOAD_GCM_TAG];
	int error;

	error = offload_validate(req);
	if (error)
		return error;
	if (!ctx->key_len)
		return -EINVAL;

	src = memdup_user((void __user *)(uintptr_t)req->src_ptr, req->data_len);
	if (IS_ERR(src))
		return PTR_ERR(src);
	if (req->aad_len) {
		aad = memdup_user((void __user *)(uintptr_t)req->aad_ptr, req->aad_len);
		if (IS_ERR(aad)) {
			error = PTR_ERR(aad);
			aad = NULL;
			goto free_src;
		}
	}
	dst = kmalloc(req->data_len, GFP_KERNEL);
	if (!dst) {
		error = -ENOMEM;
		goto free_aad;
	}

	error = ce_gcm_encrypt(ce, ctx->key, ctx->key_len, req->iv,
			       aad, req->aad_len, src, req->data_len, dst, tag);
	if (!error) {
		if (copy_to_user((void __user *)(uintptr_t)req->dst_ptr, dst,
				 req->data_len) ||
		    copy_to_user((void __user *)(uintptr_t)req->dst_ptr +
					 req->data_len,
				 tag, ONEKVM_OFFLOAD_GCM_TAG))
			error = -EFAULT;
		else {
			atomic64_inc(&offload_tx_packets);
			atomic64_add(req->data_len, &offload_tx_bytes);
		}
	}
	kfree_sensitive(dst);
free_aad:
	kfree_sensitive(aad);
free_src:
	kfree_sensitive(src);
	return error;
}

static long offload_set_key(struct offload_ctx *ctx, void __user *arg)
{
	struct onekvm_offload_key_config cfg;

	if (copy_from_user(&cfg, arg, sizeof(cfg)))
		return -EFAULT;
	if (cfg.version != ONEKVM_OFFLOAD_ABI ||
	    (cfg.key_len != 16 && cfg.key_len != 24 && cfg.key_len != 32) ||
	    cfg.auth_size != ONEKVM_OFFLOAD_GCM_TAG)
		return -EINVAL;
	memcpy(ctx->key, cfg.key, cfg.key_len);
	ctx->key_len = cfg.key_len;
	memzero_explicit(cfg.key, sizeof(cfg.key));
	return 0;
}

static long offload_encrypt(struct rk_ce *ce, struct offload_ctx *ctx,
			    void __user *arg)
{
	struct onekvm_offload_encrypt_request req;
	s64 started = ktime_get_ns();
	int error;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	mutex_lock(&ce->lock);
	error = offload_encrypt_one(ce, ctx, &req);
	mutex_unlock(&ce->lock);
	atomic64_add(ktime_get_ns() - started, &offload_crypto_ns);
	return error;
}

static long offload_encrypt_batch(struct rk_ce *ce, struct offload_ctx *ctx,
				  void __user *arg)
{
	struct onekvm_offload_batch_request batch;
	struct onekvm_offload_encrypt_request *reqs;
	s64 started = ktime_get_ns();
	unsigned int i;
	int error = 0;

	if (copy_from_user(&batch, arg, sizeof(batch)))
		return -EFAULT;
	if (batch.version != ONEKVM_OFFLOAD_ABI || batch.flags ||
	    batch.completed || !batch.count ||
	    batch.count > ONEKVM_OFFLOAD_MAX_BATCH || !batch.requests_ptr)
		return -EINVAL;
	reqs = memdup_user((void __user *)(uintptr_t)batch.requests_ptr,
			   array_size(batch.count, sizeof(*reqs)));
	if (IS_ERR(reqs))
		return PTR_ERR(reqs);

	mutex_lock(&ce->lock);
	for (i = 0; i < batch.count; i++) {
		error = offload_encrypt_one(ce, ctx, &reqs[i]);
		if (error)
			break;
		batch.completed++;
	}
	mutex_unlock(&ce->lock);
	atomic64_inc(&offload_batch_calls);
	atomic64_add(ktime_get_ns() - started, &offload_crypto_ns);
	kfree_sensitive(reqs);
	if (copy_to_user(arg, &batch, sizeof(batch)) && !error)
		error = -EFAULT;
	return error;
}

static long offload_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct offload_ctx *ctx = file->private_data;
	struct rk_ce *ce = g_ce;
	void __user *uarg = (void __user *)arg;
	long result;

	if (!ce)
		return -ENODEV;
	switch (cmd) {
	case ONEKVM_OFFLOAD_SET_KEY:
		result = offload_set_key(ctx, uarg);
		break;
	case ONEKVM_OFFLOAD_ENCRYPT:
		result = offload_encrypt(ce, ctx, uarg);
		if (result)
			atomic64_inc(&offload_tx_errors);
		break;
	case ONEKVM_OFFLOAD_ENCRYPT_BATCH:
		result = offload_encrypt_batch(ce, ctx, uarg);
		if (result)
			atomic64_inc(&offload_tx_errors);
		break;
	default:
		result = -ENOTTY;
		break;
	}
	return result;
}

static int offload_open(struct inode *inode, struct file *file)
{
	struct offload_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	file->private_data = ctx;
	return 0;
}

static int offload_release(struct inode *inode, struct file *file)
{
	struct offload_ctx *ctx = file->private_data;

	if (ctx) {
		memzero_explicit(ctx, sizeof(*ctx));
		kfree(ctx);
	}
	file->private_data = NULL;
	return 0;
}

static const struct file_operations offload_fops = {
	.owner = THIS_MODULE,
	.open = offload_open,
	.release = offload_release,
	.unlocked_ioctl = offload_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = offload_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct miscdevice offload_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = ONEKVM_OFFLOAD_NAME,
	.fops = &offload_fops,
	.mode = 0600,
};

static ssize_t tx_packets_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&offload_tx_packets));
}
static DEVICE_ATTR_RO(tx_packets);

static ssize_t tx_bytes_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&offload_tx_bytes));
}
static DEVICE_ATTR_RO(tx_bytes);

static ssize_t tx_errors_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&offload_tx_errors));
}
static DEVICE_ATTR_RO(tx_errors);

static ssize_t crypto_ns_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&offload_crypto_ns));
}
static DEVICE_ATTR_RO(crypto_ns);

static struct attribute *offload_attrs[] = {
	&dev_attr_tx_packets.attr,
	&dev_attr_tx_bytes.attr,
	&dev_attr_tx_errors.attr,
	&dev_attr_crypto_ns.attr,
	NULL,
};
ATTRIBUTE_GROUPS(offload);

static int rk_ce_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk_ce *ce;
	u32 aes_ver;
	int error;

	ce = devm_kzalloc(dev, sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return -ENOMEM;
	ce->dev = dev;
	mutex_init(&ce->lock);
	ce->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ce->reg))
		return PTR_ERR(ce->reg);

	ce->clks[0].id = "aclk";
	ce->clks[1].id = "hclk";
	ce->clks[2].id = "sclk";
	ce->clks[3].id = "pka";
	ce->nclks = 4;
	error = devm_clk_bulk_get(dev, ce->nclks, ce->clks);
	if (error)
		return error;
	error = clk_bulk_prepare_enable(ce->nclks, ce->clks);
	if (error)
		return error;

	ce->rst = devm_reset_control_get_optional_exclusive(dev, "crypto-rst");
	if (IS_ERR(ce->rst)) {
		error = PTR_ERR(ce->rst);
		goto disable_clk;
	}
	if (ce->rst) {
		reset_control_assert(ce->rst);
		udelay(10);
		reset_control_deassert(ce->rst);
	}

	error = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (error)
		goto disable_clk;

	ce->lli = dmam_alloc_coherent(dev, sizeof(*ce->lli) * 2, &ce->lli_dma,
				      GFP_KERNEL);
	ce->aad = dmam_alloc_coherent(dev, ONEKVM_OFFLOAD_MAX_AAD, &ce->aad_dma,
				      GFP_KERNEL);
	ce->src = dmam_alloc_coherent(dev, ONEKVM_OFFLOAD_MAX_INPUT, &ce->src_dma,
				      GFP_KERNEL);
	ce->dst = dmam_alloc_coherent(dev, ONEKVM_OFFLOAD_MAX_INPUT, &ce->dst_dma,
				      GFP_KERNEL);
	if (!ce->lli || !ce->aad || !ce->src || !ce->dst) {
		error = -ENOMEM;
		goto disable_clk;
	}

	ce_write(ce, CE_CLK_CTL, CE_AUTO_CLKGATE_EN | (CE_AUTO_CLKGATE_EN << 16));
	aes_ver = ce_read(ce, CE_AES_VERSION);
	if (!(aes_ver & CE_GCM_FLAG))
		dev_warn(dev, "AES_VERSION 0x%x has no GCM flag\n", aes_ver);

	offload_dev.groups = offload_groups;
	error = misc_register(&offload_dev);
	if (error)
		goto disable_clk;

	platform_set_drvdata(pdev, ce);
	g_ce = ce;
	dev_info(dev, "CE v3 AES-GCM offload aes_ver=0x%x\n", aes_ver);
	return 0;

disable_clk:
	clk_bulk_disable_unprepare(ce->nclks, ce->clks);
	return error;
}

static void rk_ce_remove(struct platform_device *pdev)
{
	struct rk_ce *ce = platform_get_drvdata(pdev);

	g_ce = NULL;
	misc_deregister(&offload_dev);
	clk_bulk_disable_unprepare(ce->nclks, ce->clks);
}

static const struct of_device_id rk_ce_of_match[] = {
	{ .compatible = "rockchip,crypto-v3" },
	{}
};
MODULE_DEVICE_TABLE(of, rk_ce_of_match);

static struct platform_driver rk_ce_driver = {
	.probe = rk_ce_probe,
	.remove = rk_ce_remove,
	.driver = {
		.name = "rk-crypto-v3",
		.of_match_table = rk_ce_of_match,
	},
};
module_platform_driver(rk_ce_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rockchip Crypto Engine v3 AES-GCM / OneKVM SRTP offload");
MODULE_AUTHOR("OneKVM");
