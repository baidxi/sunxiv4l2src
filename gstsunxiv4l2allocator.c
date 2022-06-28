#include <string.h>
#include <stdlib.h>
#include <linux/videodev2.h>

#include <gst/gstmemory.h>

#include "gstsunxiv4l2allocator.h"

GST_DEBUG_CATEGORY_STATIC(sunxiv4l2_allocator_debug);
#define GST_CAT_DEFAULT sunxiv4l2_allocator_debug

#define sunxi_v4l2src_allocator_parent_class parent_class
G_DEFINE_TYPE (GstAllocatorSunxiV4l2, gst_allocator_sunxiv4l2, GST_TYPE_ALLOCATOR);

static void
sunxi_v4l2_free_memory(GstAllocator *allocator, GstMemory *memory)
{
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;
}

static GstMemory *
sunxi_v4l2_alloc_memory(GstAllocator *allocator, gsize size,
                              GstAllocationParams *params)
{
    GstMemory *mem;
    struct v4l2_buffer v4l2_buf;
    gint nplanes;
    gint i;

    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;

    if (!v4l2_allocator->buffer_count) {
        if (!ctx->callback) {
            GST_ERROR("allocator callback not implementation.");
            return NULL;
        }
        if (ctx->callback(ctx->user_data, &v4l2_allocator->buffer_count) < 0) {
            GST_ERROR("do allocator callback failed.");
            return NULL;
        }
    }

    GST_DEBUG("allocate buffer index(%d), total count(%d)", v4l2_allocator->allocated, v4l2_allocator->buffer_count);

    if (v4l2_allocator->initialized == TRUE) {
        if (g_list_length(v4l2_allocator->mem_list) > 0) {
            GList *list = g_list_first(v4l2_allocator->mem_list);
            mem = list->data;
        }
    } else if (v4l2_allocator->allocated < v4l2_allocator->buffer_count) {

        nplanes = gst_sunxi_v4l2_allocate_buffer(ctx->v4l2_handle, v4l2_allocator->allocated, &v4l2_buf);

        if (nplanes < 0) {
            GST_ERROR("allocate buffer %d FAILED", v4l2_allocator->allocated);
            return NULL;
        }

        mem = gst_allocator_alloc(NULL, 0, NULL);

        if (!mem) {
            GST_ERROR("allocate new memory failed,errno:%d.", errno);
            if (nplanes)
                free(v4l2_buf.m.planes);

            return NULL;
        }

        mem->maxsize = nplanes ? v4l2_buf.m.planes[0].length : v4l2_buf.length;
        mem->size = nplanes ? v4l2_buf.m.planes[0].length : v4l2_buf.length;
        mem->allocator =gst_object_ref(allocator);
        
        v4l2_allocator->v4l2_buf[v4l2_allocator->allocated] = v4l2_buf;
        v4l2_allocator->allocated++;
        v4l2_allocator->mem_list = g_list_append(v4l2_allocator->mem_list, mem);
    } else {
        GST_ERROR("No more v4l2 buffer for allocating.");
        return NULL;
    }

    return mem;
}

static gpointer
sunxi_v4l2_mem_map_full(GstMemory *mem, GstMapInfo * info, gsize maxsize)
{
    gpointer data;
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;

    if (g_list_length(v4l2_allocator->blk_list) > 0) {
        GList *list = g_list_first(v4l2_allocator->blk_list);

        data = gst_sunxiv4l2_camera_pick_buffer(list->data, 0, ctx->v4l2_handle);
    }

    return data;
}

static void
sunxi_v4l2_mem_unmap_full(GstMemory *mem, GstMapInfo * info)
{
    GList *list;
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;

    list = g_list_first(v4l2_allocator->blk_list);

    gst_sunxiv4l2_camera_ref_buffer(list->data, 0, ctx->v4l2_handle);
}

static void
gst_allocator_sunxiv4l2_init(GstAllocatorSunxiV4l2 *allocator)
{
    GstAllocator *alloc;
    alloc = GST_ALLOCATOR(allocator);

    GST_DEBUG("init");

    memset(&allocator->ctx, 0, sizeof(SUNXIV4l2AllocatorContext));

    allocator->initialized = FALSE;

    alloc->mem_map_full = sunxi_v4l2_mem_map_full;
    alloc->mem_unmap_full = sunxi_v4l2_mem_unmap_full;

}

static void
gst_allocator_sunxiv4l2_class_init(GstAllocatorSunxiV4l2Class *klass)
{
    GstAllocatorClass *allocator_class;

    allocator_class = GST_ALLOCATOR_CLASS(klass);
    allocator_class->free = sunxi_v4l2_free_memory;
    allocator_class->alloc = sunxi_v4l2_alloc_memory;
}

GstAllocator *
gst_sunxi_v4l2_allocator_new(SUNXIV4l2AllocatorContext *ctx)
{
    GstAllocatorSunxiV4l2 *allocator;
    GST_DEBUG_CATEGORY_INIT(sunxiv4l2_allocator_debug, "sunxiv4l2_allocator", 0, "SUNXI V4L2 allocator");

    allocator = g_object_new(gst_allocator_sunxiv4l2_get_type(), NULL);

    if (!allocator) {
        g_print("new sunxi v4l2 alloctor failed.\n");
        return NULL;
    }

    allocator = gst_object_ref_sink(allocator);

    memcpy(&allocator->ctx, ctx, sizeof(*ctx));

    return (GstAllocator *)allocator;
}

GstFlowReturn
gst_sunxi_v4l2_buffer_register(GstAllocator *allocator)
{
    gint i;
    GstFlowReturn ret = GST_FLOW_ERROR;
    gpointer data;
    struct v4l2_buffer *v4l2_buf;
    SUNXIV4l2AllocatorContext *ctx;
    GstAllocatorSunxiV4l2 *sunxi_allocator = GST_ALLOCATOR_SUNXIV4L2(allocator);

    ctx = &sunxi_allocator->ctx;

    g_return_val_if_fail(sunxi_allocator->allocated != 0, ret);

    for (i = 0; i < sunxi_allocator->allocated; i++) {
        v4l2_buf = &sunxi_allocator->v4l2_buf[i];
        ret = gst_sunxi_v4l2_memory_map_full(ctx->v4l2_handle, v4l2_buf, &data, GST_MAP_READWRITE);

        g_return_val_if_fail(ret == GST_FLOW_OK, ret);

        sunxi_allocator->blk_list =  g_list_append(sunxi_allocator->blk_list, data);
    }

    sunxi_allocator->initialized = TRUE;

    return ret;
}