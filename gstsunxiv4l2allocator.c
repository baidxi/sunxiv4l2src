#include <string.h>
#include <stdlib.h>
#include <linux/videodev2.h>
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
    GST_DEBUG("%s", __func__);

    if (v4l2_allocator->allocated < 0)
        return;

    gst_sunxi_v4l2_free_buffer(ctx->v4l2_handle, v4l2_allocator->allocated);
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

    if (v4l2_allocator->allocated < v4l2_allocator->buffer_count) {
        nplanes = gst_sunxi_v4l2_allocate_buffer(ctx->v4l2_handle, v4l2_allocator->allocated, &v4l2_buf);

        if (nplanes < 0) {
            GST_ERROR("allocate buffer %d FAILED", v4l2_allocator->allocated);
            return NULL;
        }

        v4l2_allocator->v4l2_buf[v4l2_allocator->allocated] = v4l2_buf;
        v4l2_allocator->allocated++;
    } else {
        GST_ERROR("No more v4l2 buffer for allocating.");
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
    mem->offset = v4l2_allocator->allocated - 1;
    mem->size = nplanes ? v4l2_buf.m.planes[0].length : v4l2_buf.length;
    mem->allocator = allocator;

    return mem;
}

static gpointer
sunxi_v4l2_mem_map(GstMemory *mem, gsize maxsize, GstMapFlags flags)
{
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    gpointer ret;
    struct v4l2_buffer *buf = &v4l2_allocator->v4l2_buf[mem->offset];

    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;

    GST_DEBUG("%s", __func__);

    ret =  gst_sunxi_v4l2_memory_map(ctx->v4l2_handle, buf, mem->offset, flags);

    if (!ret)
        return NULL;

    mem = gst_memory_ref(mem);

    return ret;
}

static void
sunxi_v4l2_unmap(GstMemory *mem)
{
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;
    struct v4l2_buffer *buf = &v4l2_allocator->v4l2_buf[mem->offset];
    GST_DEBUG("%s", __func__);

    gst_sunxi_v4l2_memory_unmap(ctx->v4l2_handle, mem->offset, buf);
    
    gst_memory_unref(mem);

}

// static GstMemory *
// sunxi_v4l2_memcpy(GstMemory *mem, gssize offset, gssize size)
// {

// }

static gpointer
sunxi_v4l2_mem_map_full(GstMemory *mem, GstMapInfo * info, gsize maxsize)
{
    gpointer data;
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    struct v4l2_buffer *buf = &v4l2_allocator->v4l2_buf[mem->offset];
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;
    gsize size;

    data = gst_sunxi_v4l2_memory_map_full(ctx->v4l2_handle, buf, mem->offset, &size, GST_MAP_READWRITE);

    if (!data)
        return NULL;

    info->data = data;
    info->size = size;
    info->memory = mem;
    info->maxsize = size;

    mem = gst_memory_ref(mem);

    return data;
}

static void
sunxi_v4l2_mem_unmap_full(GstMemory *mem, GstMapInfo * info)
{
    GstAllocatorSunxiV4l2 *v4l2_allocator = GST_ALLOCATOR_SUNXIV4L2(mem->allocator);
    SUNXIV4l2AllocatorContext *ctx = &v4l2_allocator->ctx;
    struct v4l2_buffer *buf = &v4l2_allocator->v4l2_buf[mem->offset];

    gst_sunxi_v4l2_memory_unmap(ctx->v4l2_handle, mem->offset, buf);

    gst_memory_unref(mem);

    memset(info, 0, sizeof(GstMapInfo));
}

static void
gst_allocator_sunxiv4l2_init(GstAllocatorSunxiV4l2 *allocator)
{
    GstAllocator *alloc;
    alloc = GST_ALLOCATOR(allocator);

    GST_DEBUG("init");

    memset(&allocator->ctx, 0, sizeof(SUNXIV4l2AllocatorContext));

    alloc->mem_map = sunxi_v4l2_mem_map;
    alloc->mem_unmap = sunxi_v4l2_unmap;
    // alloc->mem_copy = sunxi_v4l2_memcpy;
    alloc->mem_map_full = sunxi_v4l2_mem_map_full;
    alloc->mem_unmap_full = sunxi_v4l2_mem_unmap_full;
    // alloc->mem_share = sunxi_v4l2_share_memory;
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

    GST_DEBUG("%s", __func__);

    allocator = g_object_new(gst_allocator_sunxiv4l2_get_type(), NULL);

    if (!allocator) {
        g_print("new sunxi v4l2 alloctor failed.\n");
        return NULL;
    }

    allocator = gst_object_ref_sink(allocator);

    memcpy(&allocator->ctx, ctx, sizeof(*ctx));

    // gst_allocator_register("sunxi_v4l2_alloctor", gst_object_ref(allocator));

    return (GstAllocator *)allocator;
}

