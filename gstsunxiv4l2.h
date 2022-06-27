#ifndef _GST_SUNXIV4L2_H
#define _GST_SUNXIV4L2_H

#include <linux/videodev2.h>

#include <gst/gst.h>
#include <gst/video/video-info.h>

enum v4l2_sensor_type {
    V4L2_SENSOR_TYPE_YUV = 0,
    V4L2_SENSOR_TYPE_RAW = 1,
};

#define V4L2_CID_USER_SUNXI_CAMERA_BASE     (V4L2_CID_USER_BASE + 0x1050)
#define V4L2_CID_SENSOR_TYPE                (V4L2_CID_USER_SUNXI_CAMERA_BASE + 14)

#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))

GstCaps *gst_sunxiv4l2_get_device_caps(gint type);
gpointer gst_sunxiv4l2_open_device(gchar *device, int type);
GstCaps *gst_sunxiv4l2_get_caps(gpointer v4l2handle);
gint gst_sunxiv4l2_close_device(gpointer handle);
gint gst_sunxi_v4l2_set_buffer_count(gpointer v4l2handle, guint count, guint memory_mode);
gint gst_sunxi_v4l2_allocate_buffer(gpointer v4l2handle, gint idx, struct v4l2_buffer *buf);
gint gst_sunxi_v4l2_free_buffer(gpointer v4l2handle, gint idx);
// gpointer gst_sunxi_v4l2_memory_map(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gint offset, GstMapFlags flags);
GstFlowReturn gst_sunxi_v4l2_memory_map_full(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gpointer *data, GstMapFlags flags);
void gst_sunxi_v4l2_memory_unmap(gpointer v4l2handle, gint idx, struct v4l2_buffer *buf);
gboolean gst_sunxi_v4l2_streamon(gpointer v4l2handle);
gint gst_sunxi_v4l2_streamoff(gpointer v4l2handle);
guint gst_sunxiv_v4l2_fmt_gst2v4l2(GstVideoFormat gstfmt);
void gst_sunxiv4l2_set_camera_index(gpointer v4l2handle, gint idx);
gint gst_sunxiv4l2_get_camera_index(gpointer v4l2handle);
gint gst_sunxi_v4l2_set_format(gpointer v4l2handle, guint v4l2fmt, GstVideoInfo *info);
gint gst_sunxi_v4l2capture_config(gpointer v4l2handle, guint v4l2fmt, guint w, guint h, guint fps_n, guint fps_d);
gint gst_sunxi_video_info_from_caps(GstVideoInfo *info, GstCaps *caps);
gboolean gst_sunxiv4l2_is_open(gpointer v4l2handle);
guint gst_sunxiv4l2_fps_d(gpointer v4l2handle);
guint gst_sunxiv4l2_fps_n(gpointer v4l2handle);
GstFlowReturn gst_sunxiv4l2_flush_all_buffer(gpointer v4l2handle);
gint gst_sunxiv4l2_camera_qbuf(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gint idx);
gint gst_sunxiv4l2_camera_dqbuf(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gint idx);

gpointer gst_sunxiv4l2_camera_pick_buffer(gpointer data, gint idx, gpointer v4l2handle);
void gst_sunxiv4l2_camera_ref_buffer(gpointer data, gint idx, gpointer v4l2handle);

#endif