#ifndef __GSTSUNXI_V4L2_ALLOCATOR_H_
#define __GSTSUNXI_V4L2_ALLOCATOR_H_

#include <gst/allocators/allocators.h>
#include <linux/videodev2.h>
#include "gstsunxiv4l2.h"

#define GST_TYPE_ALLOCATOR_SUNXIV4L2    (gst_allocator_sunxiv4l2_get_type())
#define GST_ALLOCATOR_SUNXIV4L2(obj)    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_ALLOCATOR_SUNXIV4L2, GstAllocatorSunxiV4l2))
#define GST_ALLOCATOR_SUNXIV4L2_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_ALLOCV4L2_CLASS, GstAllocatorSuperclass))
#define GST_IS_ALLOCATOR_SUNXIV4L2(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_ALLOCATOR_SUNXIV4L2))
#define GST_IS_ALLOCATOR_SUNXVI4L2_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_ALLOCATOR_SUNXIV4L2))

typedef gint (*SUNXIV4l2AllocatorCb)(gpointer user_data, gint *buffer_count);

typedef struct _GstAllocatorSunxiV4l2Class GstAllocatorSunxiV4l2Class;
typedef struct _GstAllocatorSunxiV4l2 GstAllocatorSunxiV4l2;

typedef struct {
    gpointer v4l2_handle;
    gpointer user_data;
    SUNXIV4l2AllocatorCb callback;
}SUNXIV4l2AllocatorContext;

struct _GstAllocatorSunxiV4l2{
    GstAllocator parent;
    const gchar *name;
    gboolean initialized;
    SUNXIV4l2AllocatorContext ctx;
    gint buffer_count;
    struct v4l2_buffer v4l2_buf[3];
    GList  *mem_list;
    GList *blk_list;
    // gboolean in_used[3];
    // gint mapped;
    gint allocated; 
    // gpointer priv[3];
    // GstMemory *mem[3];
};

struct _GstAllocatorSunxiV4l2Class {
    GstAllocatorClass parent_class;
};
    // GstallocatorPhyMemClass parent_class;


GType gst_allocator_sunxiv4l2_get_type(void);

GstAllocator *gst_sunxi_v4l2_allocator_new(SUNXIV4l2AllocatorContext *ctx);
GstFlowReturn gst_sunxi_v4l2_buffer_register(GstAllocator *allocator);

#endif