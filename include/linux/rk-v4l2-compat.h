/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 5.10 Rockchip CIF/DPHY helpers used when building against Linux 6.18.
 * Included first via ccflags -include so vendor sources keep their old names.
 */

#ifndef _LINUX_RK_V4L2_COMPAT_H
#define _LINUX_RK_V4L2_COMPAT_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fwnode.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/proc_fs.h>
#include <linux/property.h>
#include <linux/string.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>

#define strlcpy(dst, src, size)		strscpy((dst), (src), (size))
#define PDE_DATA(inode)			pde_data(inode)
#define del_timer_sync			timer_delete_sync
#define del_timer			timer_delete
#define media_entity_remote_pad	media_pad_remote_pad_first
#define v4l2_async_notifier_cleanup	v4l2_async_nf_cleanup
#define v4l2_async_notifier_unregister	v4l2_async_nf_unregister
#define v4l2_fwnode_bus_parallel		v4l2_mbus_config_parallel
#define v4l2_fwnode_bus_mipi_csi2	v4l2_mbus_config_mipi_csi2
#define min_buffers_needed		min_queued_buffers
#define v4l2_subdev_pad_config		v4l2_subdev_state

/*
 * Define after v4l2-async.h so struct v4l2_async_subdev_endpoint is parsed
 * with its real name. Vendor code then uses the 5.10 struct tag.
 */
#define v4l2_async_subdev		v4l2_async_connection
#define v4l2_async_register_subdev_sensor_common \
	v4l2_async_register_subdev_sensor

#ifndef V4L2_MBUS_CSI2_1_LANE
#define V4L2_MBUS_CSI2_1_LANE		BIT(0)
#define V4L2_MBUS_CSI2_2_LANE		BIT(1)
#define V4L2_MBUS_CSI2_3_LANE		BIT(2)
#define V4L2_MBUS_CSI2_4_LANE		BIT(3)
#define V4L2_MBUS_CSI2_CHANNEL_0		BIT(4)
#define V4L2_MBUS_CSI2_CHANNEL_1		BIT(5)
#define V4L2_MBUS_CSI2_CHANNEL_2		BIT(6)
#define V4L2_MBUS_CSI2_CHANNEL_3		BIT(7)
#define V4L2_MBUS_CSI2_CONTINUOUS_CLOCK	BIT(8)
#define V4L2_MBUS_CSI2_LANES		(V4L2_MBUS_CSI2_1_LANE | \
					 V4L2_MBUS_CSI2_2_LANE | \
					 V4L2_MBUS_CSI2_3_LANE | \
					 V4L2_MBUS_CSI2_4_LANE)
#define V4L2_MBUS_CSI2_CHANNELS		(V4L2_MBUS_CSI2_CHANNEL_0 | \
					 V4L2_MBUS_CSI2_CHANNEL_1 | \
					 V4L2_MBUS_CSI2_CHANNEL_2 | \
					 V4L2_MBUS_CSI2_CHANNEL_3)
#endif

static inline unsigned int
rk_v4l2_csi2_flags_from_lanes(unsigned int lanes)
{
	unsigned int flags = V4L2_MBUS_CSI2_CHANNEL_0;

	if (lanes >= 1 && lanes <= 4)
		flags |= BIT(lanes - 1);
	return flags;
}

static inline void
rk_v4l2_mbus_sync_csi2_flags(struct v4l2_mbus_config *mbus)
{
	unsigned int lanes;

	if (!mbus)
		return;
	if (mbus->type != V4L2_MBUS_CSI2_DPHY &&
	    mbus->type != V4L2_MBUS_CSI2_CPHY)
		return;
	if (mbus->flags & V4L2_MBUS_CSI2_LANES)
		return;
	lanes = mbus->bus.mipi_csi2.num_data_lanes;
	if (lanes >= 1 && lanes <= 4)
		mbus->flags |= BIT(lanes - 1);
	mbus->flags |= V4L2_MBUS_CSI2_CHANNEL_0;
}

static inline void v4l2_async_notifier_init(struct v4l2_async_notifier *n)
{
	memset(n, 0, sizeof(*n));
	INIT_LIST_HEAD(&n->waiting_list);
	INIT_LIST_HEAD(&n->done_list);
	INIT_LIST_HEAD(&n->notifier_entry);
}

static inline int
v4l2_async_notifier_register(struct v4l2_device *v4l2_dev,
			     struct v4l2_async_notifier *n)
{
	n->v4l2_dev = v4l2_dev;
	n->sd = NULL;
	return v4l2_async_nf_register(n);
}

static inline int
v4l2_async_subdev_notifier_register(struct v4l2_subdev *sd,
				     struct v4l2_async_notifier *n)
{
	n->sd = sd;
	n->v4l2_dev = NULL;
	return v4l2_async_nf_register(n);
}

static inline int
__rk_v4l2_async_parse_fwnode(struct device *dev,
			     struct v4l2_async_notifier *notifier,
			     size_t asd_struct_size, unsigned int port,
			     bool has_port,
			     int (*parse)(struct device *dev,
					  struct v4l2_fwnode_endpoint *vep,
					  struct v4l2_async_subdev *asd))
{
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	if (asd_struct_size < sizeof(struct v4l2_async_connection))
		asd_struct_size = sizeof(struct v4l2_async_connection);

	while ((ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), ep))) {
		struct v4l2_fwnode_endpoint vep = { .bus_type = 0 };
		struct fwnode_handle *remote;
		struct v4l2_async_connection *asc;

		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		if (ret) {
			fwnode_handle_put(ep);
			return ret;
		}

		if (has_port && vep.base.port != port)
			continue;

		remote = fwnode_graph_get_remote_port_parent(ep);
		if (!remote)
			continue;
		if (remote == dev_fwnode(dev)) {
			fwnode_handle_put(remote);
			continue;
		}

		asc = __v4l2_async_nf_add_fwnode(notifier, remote,
						  asd_struct_size);
		fwnode_handle_put(remote);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			if (ret == -EEXIST) {
				ret = 0;
				continue;
			}
			fwnode_handle_put(ep);
			return ret;
		}

		if (parse) {
			ret = parse(dev, &vep, asc);
			if (ret) {
				fwnode_handle_put(ep);
				return ret;
			}
		}
	}

	return 0;
}

static inline int
v4l2_async_notifier_parse_fwnode_endpoints(struct device *dev,
					   struct v4l2_async_notifier *notifier,
					   size_t asd_struct_size,
					   int (*parse)(struct device *dev,
							struct v4l2_fwnode_endpoint *vep,
							struct v4l2_async_subdev *asd))
{
	return __rk_v4l2_async_parse_fwnode(dev, notifier, asd_struct_size,
					     0, false, parse);
}

static inline int
v4l2_async_notifier_parse_fwnode_endpoints_by_port(struct device *dev,
						   struct v4l2_async_notifier *notifier,
						   size_t asd_struct_size,
						   unsigned int port,
						   int (*parse)(struct device *dev,
								struct v4l2_fwnode_endpoint *vep,
								struct v4l2_async_subdev *asd))
{
	return __rk_v4l2_async_parse_fwnode(dev, notifier, asd_struct_size,
					     port, true, parse);
}

static inline void v4l2_async_notifier_clr_unready_dev(struct v4l2_async_notifier *n)
{
}

static inline void vb2_ops_wait_prepare(struct vb2_queue *vq)
{
	if (vq->lock)
		mutex_unlock(vq->lock);
}

static inline void vb2_ops_wait_finish(struct vb2_queue *vq)
{
	if (vq->lock)
		mutex_lock(vq->lock);
}

static inline struct v4l2_rect *
v4l2_subdev_get_try_crop(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			 unsigned int pad)
{
	return v4l2_subdev_state_get_crop(state, pad);
}

/*
 * 6.18 moved g_frame_interval from video_ops to pad_ops and added a
 * v4l2_subdev_state argument. Vendor CIF still calls the 5.10 name.
 */
static inline int
rk_v4l2_subdev_g_frame_interval(struct v4l2_subdev *sd,
				struct v4l2_subdev_frame_interval *fi)
{
	return v4l2_subdev_call(sd, pad, get_frame_interval, NULL, fi);
}

#endif /* _LINUX_RK_V4L2_COMPAT_H */
