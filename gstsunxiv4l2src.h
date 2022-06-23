#ifndef _GSTSUNXIV4L2SRC_H_
#define _GSTSUNXIV4L2SRC_H_

#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <gst/base/gstbasesrc.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/gstvideometa.h>

G_BEGIN_DECLS

#ifndef VERSION
#define VERSION "1.0"
#endif
#ifndef PACKAGE
#define PACKAGE "sunxi v4l2 src plugin"
#endif
#ifndef ORIG
#define ORIG "https://www.dbappsecurity.com.cn"
#endif

#define GST_TYPE_SUNXI_V4L2SRC \
    (gst_sunxi_v4l2src_get_type())
#define GST_SUNXI_V4L2SRC(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_SUNXI_V4L2SRC, GstSunxiV4l2Src))
#define GST_SUNXI_V4L2SRC_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_SUNXI_V4L2SRC, GstSunxiV4l2SrcClass))
#define GST_IS_SUNXI_V4L2SRC(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_SUNXI_V4L2SRC))
#define GST_IS_SUNXI_V4L2SRC_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_SUNXI_V4L2SRC))    

#define SUNXI_GST_PLUGIN_AUTHOR "jeck.chen@dbappsecurity.com.cn"
#define DEFAULT_DEVICE "/dev/video0"

#define DEFAULT_FRAMES_IN_V4L2_CAPTURE 3

typedef struct _GstSunxiV4l2Src GstSunxiV4l2Src;
typedef struct _GstSunxiV4l2SrcClass GstSunxiV4l2SrcClass;


struct _GstSunxiV4l2Src {
    GstPushSrc element;
    gchar *device;
    gpointer v4l2handle;
    GstCaps *probed_caps;
    GstCaps *old_caps;
    GstVideoInfo info;
    guint v4l2fmt;
    GstClockTime ctrl_time;
    GstClockTime last_timestamp;
    gboolean    has_bad_timestamp;
    GstClockTime duration;
    guint64 offset;
    guint64 renegotiation_adjust;    
    guint io_mode;
    guint actual_buf_cnt;
    gboolean stream_on;
    GstVideoAlignment video_align;
    GstBufferPool *pool;
    GstAllocator *allocator;
    GList *gstbuffer_in_v4l2;
};

struct _GstSunxiV4l2SrcClass {
    GstPushSrcClass parent_class;
};

GType gst_sunxi_v4l2src_get_type(void);

G_END_DECLS

#endif