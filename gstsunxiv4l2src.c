#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include <gst/gst.h>
#include <linux/videodev2.h>
#include <gst/video/gstvideopool.h>

#include "gstsunxiv4l2.h"
#include "gstsunxiv4l2src.h"
#include "gstsunxiv4l2allocator.h"

#define DEFAULT_DEVICE "/dev/video0"
#define DEFAULT_WIDTH 320
#define DEFAULT_HEIGHT 240
#define DEFAULT_DENOMINATOR 1
#define DEFAULT_NUMERATOR 30
#define DEFAULT_FORMAT "NV21"
#define DEFAULT_SIZE (src->info.width * src->info.height * 3 / 2)

enum
{
    PROP_0,
    PROP_DEVICE,
    PROP_IOMODE,
    PROP_CAMERA_INDEX,
};

GST_DEBUG_CATEGORY_STATIC(sunxiv4l2src_debug);
#define GST_CAT_DEFAULT sunxiv4l2src_debug

#define gst_sunxi_v4l2src_parent_class parent_class
G_DEFINE_TYPE(GstSunxiV4l2Src, gst_sunxi_v4l2src, GST_TYPE_PUSH_SRC);

static void
gst_sunxiv4l2src_set_property(GObject *object, guint prop_id,
                              const GValue *value, GParamSpec *pspec)
{
    GstSunxiV4l2Src *src = GST_SUNXI_V4L2SRC(object);
    
    switch (prop_id)
    {
    case PROP_DEVICE:
        src->device = g_value_dup_string(value);
        break;
    case PROP_IOMODE:
        src->io_mode = g_value_get_uint(value);
        break;
    case PROP_CAMERA_INDEX:
        gst_sunxiv4l2_set_camera_index(src->v4l2handle, g_value_get_int(value));
        break;
    default:
        break;
    }
}

static void
gst_sunxiv4l2src_get_property(GObject *object, guint prop_id,
                              GValue *value, GParamSpec *pspec)
{
    GstSunxiV4l2Src *src = GST_SUNXI_V4L2SRC(object);

    switch (prop_id)
    {
    case PROP_DEVICE:
        g_value_set_string(value, src->device);
        break;
    case PROP_IOMODE:
        g_value_set_uint(value, src->io_mode);
        break;
    case PROP_CAMERA_INDEX:
        g_value_set_int(value, gst_sunxiv4l2_get_camera_index(src->v4l2handle));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static GstCaps *
gst_sunxi_v4l2src_get_device_caps(GstBaseSrc *src)
{
    GstSunxiV4l2Src *v4l2src = NULL;
    GstCaps *caps = NULL;

    v4l2src = GST_SUNXI_V4L2SRC(src);

    if (v4l2src->v4l2handle == NULL) {
        return gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(v4l2src));
    }

    if (v4l2src->probed_caps)
        return gst_caps_ref(v4l2src->probed_caps);

    caps = gst_sunxiv4l2_get_caps(v4l2src->v4l2handle);

    if (!caps)
        GST_WARNING_OBJECT(v4l2src, "Can't get caps from device.");

    v4l2src->probed_caps = gst_caps_ref(caps);

    GST_INFO("probed caps: %" GST_PTR_FORMAT, caps);

    return caps;
}

static gboolean
gst_sunxiv4l2src_set_caps(GstBaseSrc *bsrc, GstCaps *caps)
{
    const GstStructure *structure;
    GstSunxiV4l2Src *v4l2src;
    GstVideoInfo info;
    gint v4l2_fmt;

    v4l2src = GST_SUNXI_V4L2SRC(bsrc);

    GST_DEBUG("caps:%"GST_PTR_FORMAT, caps);

    if (v4l2src->old_caps) {
        if (gst_caps_is_equal(v4l2src->old_caps, caps))
            return TRUE;
    }

    if (gst_sunxi_video_info_from_caps(&info, caps)) {
        GST_ERROR("invalid caps. %"GST_PTR_FORMAT, caps);
        return FALSE;
    }

    v4l2_fmt = gst_sunxiv_v4l2_fmt_gst2v4l2(GST_VIDEO_INFO_FORMAT(&info));

    if (!v4l2_fmt) {
        GST_ERROR_OBJECT(v4l2src, "%s not supported", GST_VIDEO_INFO_NAME(&info));
        return FALSE;
    }

    GST_DEBUG("[%c%c%c%c]", v4l2_fmt & 0xff, (v4l2_fmt >> 8) & 0xff, 
                (v4l2_fmt >> 16) & 0xff, (v4l2_fmt >> 24) & 0xff);
    GstVideoFormat gst_fmt = gst_video_format_from_fourcc(v4l2_fmt);

    v4l2src->v4l2fmt = v4l2_fmt;

    memcpy(&v4l2src->info, &info, sizeof(info));

    /* FIXME Add device reset*/

    if (v4l2src->old_caps) {
        gst_caps_unref(v4l2src->old_caps);
        v4l2src->old_caps = NULL;
    }

    v4l2src->old_caps = gst_caps_copy(caps);

    return TRUE;
}

static GstCaps *
gst_sunxiv4l2src_fixate(GstBaseSrc *bsrc, GstCaps *caps)
{
    caps = GST_BASE_SRC_CLASS(parent_class)->fixate(bsrc, caps);

    return caps;
}

static GstCaps *
gst_sunxiv4l2src_get_caps(GstBaseSrc *bsrc, GstCaps *filter)
{
    GstCaps *caps = NULL;

    caps = gst_sunxi_v4l2src_get_device_caps(bsrc);

    if (caps && filter)
    {
        GstCaps *intersection;

        intersection = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        caps = intersection;
    }

    return caps;
}

static gboolean
gst_sunxiv4l2src_query(GstBaseSrc *bsrc, GstQuery *query)
{
    GstSunxiV4l2Src *src;
    gboolean ret = FALSE;

    src = GST_SUNXI_V4L2SRC(bsrc);

    switch(GST_QUERY_TYPE(query)) {
        case GST_QUERY_LATENCY: {
            GstClockTime min_latency, max_latency;
            guint32 fps_n, fps_d;
            guint num_buffers = 0;
            GstStructure *config;
            if (!gst_sunxiv4l2_is_open(src->v4l2handle)) {
                GST_WARNING("Can't give latency since device isn't open!");
                goto done;
            }

            fps_n = gst_sunxiv4l2_fps_n(src->v4l2handle);
            fps_d = gst_sunxiv4l2_fps_d(src->v4l2handle);

            if (fps_n <= 0 || fps_d <= 0) {
                GST_WARNING("Can't give latency since framerate isn't fixated !");
                goto done;
            }

            min_latency = gst_util_uint64_scale_int(GST_SECOND, fps_d, fps_n);

            if (src->pool != NULL) {
                config = gst_buffer_pool_get_config(src->pool);
                gst_buffer_pool_config_get_params(config, NULL, NULL, NULL, &num_buffers);
            }

            if (num_buffers == 0)
                max_latency = -1;
            else
                max_latency = num_buffers * min_latency;

            GST_DEBUG("report latency min %" GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
                GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency));

            gst_query_set_latency(query, TRUE, min_latency, max_latency);

            ret = TRUE;                
        }
        break;
        default:
            ret = GST_BASE_SRC_CLASS(parent_class)->query(bsrc, query);
            break;
    }

done:
    return ret;
}

static gint
gst_sunxi_v4l2src_config(GstSunxiV4l2Src *v4l2src)
{
    guint w, h;

    w = v4l2src->info.width + v4l2src->video_align.padding_left + v4l2src->video_align.padding_right;
    h = v4l2src->info.height + v4l2src->video_align.padding_top + v4l2src->video_align.padding_bottom;

    GST_DEBUG("%d * %d pading: (%d, %d), (%d, %d) fps:%d/%d",
        w,h,
        v4l2src->video_align.padding_left, v4l2src->video_align.padding_top,
        v4l2src->video_align.padding_right, v4l2src->video_align.padding_bottom,
        v4l2src->info.fps_n, v4l2src->info.fps_d);

    return gst_sunxi_v4l2capture_config(v4l2src->v4l2handle, v4l2src->v4l2fmt, w, h, v4l2src->info.fps_n, v4l2src->info.fps_d);
}

static gint
gst_sunxi_v4l2_callocator_cb(gpointer user_data, gint *buffer_count)
{
    GstSunxiV4l2Src *v4l2src = GST_SUNXI_V4L2SRC(user_data);
    gint ret;
    guint min, max;

    if (!v4l2src->pool)
        v4l2src->pool = gst_base_src_get_buffer_pool(GST_BASE_SRC(v4l2src));
    if (v4l2src->pool) {
        GstStructure *config;
        config = gst_buffer_pool_get_config(v4l2src->pool);

        memset(&v4l2src->video_align, 0, sizeof(GstVideoAlignment));
        if (gst_buffer_pool_config_has_option(config, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
            gst_buffer_pool_config_get_video_alignment(config, &v4l2src->video_align);
            GST_DEBUG("pool has alignment (%d, %d), (%d, %d)",
                v4l2src->video_align.padding_left, v4l2src->video_align.padding_top,
                v4l2src->video_align.padding_right, v4l2src->video_align.padding_bottom);
        }

        gst_buffer_pool_config_get_params(config, NULL, NULL, &min, &max);
        GST_DEBUG("need allocated %d buffers.", max);

        gst_structure_free(config);

        if (gst_sunxi_v4l2src_config(v4l2src) < 0) {
            GST_ERROR_OBJECT(v4l2src, "camera configuration failed.");
            g_print("capture device: %s probed caps: %"GST_PTR_FORMAT, v4l2src->device, v4l2src->probed_caps);
            g_print("Please config accepted caps!\n");
            return -1;
        }

        ret = gst_sunxi_v4l2_set_format(v4l2src->v4l2handle, v4l2src->v4l2fmt, &v4l2src->info);

        if (ret < 0) {
            return -1;
        }

        if (gst_sunxi_v4l2_set_buffer_count(v4l2src->v4l2handle, max, v4l2src->io_mode) < 0) {
            return -1;
        }

        *buffer_count = max;

    } else {
        GST_ERROR_OBJECT(v4l2src, "no pool to get buffer count.\n");
        return -1;
    }

    return ret;
}

static GstFlowReturn
gst_sunxi_v4l2src_register_buffer(GstSunxiV4l2Src *v4l2src)
{
    GstBufferPool *pool;
    GstAllocator *allocator;
    pool = v4l2src->pool;
    GstStructure *config;

    config = gst_buffer_pool_get_config(pool);

    g_return_val_if_fail(config != NULL, GST_FLOW_ERROR);

    if (gst_buffer_pool_config_get_allocator(config, &allocator, NULL) == FALSE) {
        GST_ERROR("Get allocator FAILED");
        return GST_FLOW_ERROR;
    }

    return gst_sunxi_v4l2_buffer_register(allocator);
}

static GstFlowReturn
gst_sunxi_v4l2src_acquire_buffer(GstSunxiV4l2Src *v4l2src, GstBuffer **buf)
{
    GstFlowReturn ret = GST_FLOW_OK;
    GstVideoMeta *vmeta;
    GstVideoFrameFlags flags = GST_VIDEO_FRAME_FLAG_NONE;
    GstBufferPool *pool = v4l2src->pool;
    GstBuffer *buffer;

    if (v4l2src->stream_on == FALSE) {

        ret = gst_sunxi_v4l2src_register_buffer(v4l2src);

        g_return_val_if_fail(ret == GST_FLOW_OK, ret);

        ret = gst_sunxiv4l2_flush_all_buffer(v4l2src->v4l2handle);

        g_return_val_if_fail(ret == GST_FLOW_OK, ret);

        ret = gst_buffer_pool_acquire_buffer(pool, &buffer, NULL);

        g_return_val_if_fail(ret == GST_FLOW_OK, ret);

        v4l2src->stream_on =  gst_sunxi_v4l2_streamon(v4l2src->v4l2handle);

        g_return_val_if_fail(v4l2src->stream_on == TRUE, GST_FLOW_ERROR);
    } else {
        ret = gst_buffer_pool_acquire_buffer(pool, &buffer, NULL);

        g_return_val_if_fail(ret == GST_FLOW_OK , ret);
    }

    vmeta = gst_buffer_get_video_meta(buffer);

    if (!vmeta) {
        GstVideoInfo info;

        if (gst_sunxi_video_info_from_caps(&info, v4l2src->old_caps)) {
            GST_ERROR_OBJECT(v4l2src, "invalid caps.");
            return GST_FLOW_ERROR;
        }

        vmeta->flags = flags;

        vmeta = gst_buffer_add_video_meta(buffer, 
            GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_INFO_FORMAT(&info),
            v4l2src->info.width,
            v4l2src->info.height
        );
    }    

    *buf = buffer;

    GST_DEBUG("field type: %d", flags);

    return ret;
}

static GstFlowReturn
gst_sunxi_v4l2src_create(GstPushSrc *psrc, GstBuffer **buf)
{
    GstFlowReturn ret;
    GstClockTime delay, abs_time, timestamp, base_time, duration;
    GstClock *clock;
    GstMessage *qos_msg;
    GstSunxiV4l2Src *v4l2src = GST_SUNXI_V4L2SRC(psrc);

    ret = gst_sunxi_v4l2src_acquire_buffer(v4l2src, buf);

    if (G_UNLIKELY(ret != GST_FLOW_OK)) {
        GST_DEBUG("error process buffer %d (%s)", ret, gst_flow_get_name(ret));
        return ret;
    }

    timestamp = GST_BUFFER_TIMESTAMP(*buf);
    duration = v4l2src->duration;


    GST_OBJECT_LOCK(v4l2src);

    if ((clock = GST_ELEMENT_CLOCK(v4l2src))) {
        base_time = GST_ELEMENT(v4l2src)->base_time;
        gst_object_ref(clock);
    } else {
        base_time = GST_CLOCK_TIME_NONE;
    }

    GST_OBJECT_UNLOCK(v4l2src);

    if (clock) {
        abs_time = gst_clock_get_time(clock);
        gst_object_unref(clock);
    } else {
        abs_time = GST_CLOCK_TIME_NONE;
    }

retry:
    if (!v4l2src->has_bad_timestamp && timestamp != GST_CLOCK_TIME_NONE) {
        struct timespec now;
        GstClockTime gstnow;

        clock_gettime(CLOCK_MONOTONIC, &now);
        gstnow = GST_TIMESPEC_TO_TIME(now);

        if (timestamp > gstnow || (gstnow - timestamp) > (10 * GST_SECOND)) {
            GTimeVal now;

            g_get_current_time(&now);
            gstnow = GST_TIMEVAL_TO_TIME(now);
        }

        if (timestamp > gstnow) {
            GST_WARNING("Timestamp going backward, ignoring driver timestamps");
            v4l2src->has_bad_timestamp = TRUE;
            goto retry;
        }

        delay = gstnow - timestamp;

        if (delay > timestamp) {
            GST_WARNING("Timestamp does not correlate with any clock, ignoring driver timestamps");
            v4l2src->has_bad_timestamp = TRUE;
            goto retry;
        }

        v4l2src->last_timestamp = timestamp;

        GST_DEBUG("ts: %"GST_TIME_FORMAT " now %" GST_TIME_FORMAT " delay %"GST_TIME_FORMAT, 
            GST_TIME_ARGS(timestamp), GST_TIME_ARGS(gstnow), GST_TIME_ARGS(delay));
    } else {
        if (GST_CLOCK_TIME_IS_VALID(duration))
            delay = duration;
        else
            delay = 0;
    }

    /* set buffer metadata */

    if (G_LIKELY(abs_time != GST_CLOCK_TIME_NONE)) {
        /* the time now is the time of clock minus the base time */
        timestamp = abs_time - base_time;

        /* ajust for delay in the device */
        if (timestamp > delay)
            timestamp -= delay;
        else
            timestamp = 0;
    } else {
        timestamp = GST_CLOCK_TIME_NONE;
    }

    /* activate settings for next frame */
    if (GST_CLOCK_TIME_IS_VALID(duration)) {
        v4l2src->ctrl_time += duration;
    } else {
        /* this is not very good (as it should be the next timestamp),
         * still good enough for linear fades (as long as it is not -1)
         */
        v4l2src->ctrl_time = timestamp;
    }

    gst_object_sync_values(GST_OBJECT(psrc), v4l2src->ctrl_time);

    GST_INFO("sync to %" GST_TIME_FORMAT " out ts %" GST_TIME_FORMAT,
        GST_TIME_ARGS(v4l2src->ctrl_time), GST_TIME_ARGS(timestamp));

    if (!GST_BUFFER_OFFSET_IS_VALID(*buf) ||
        !GST_BUFFER_OFFSET_END_IS_VALID(*buf)) {
        GST_BUFFER_OFFSET(*buf) = v4l2src->offset++;
        GST_BUFFER_OFFSET_END(*buf) = v4l2src->offset;
    } else {
        GST_BUFFER_OFFSET(*buf) += v4l2src->renegotiation_adjust;
        GST_BUFFER_OFFSET_END(*buf) += v4l2src->renegotiation_adjust;

        if ((v4l2src->offset != 0) && 
            (GST_BUFFER_OFFSET(*buf) != (v4l2src->offset + 1))) {
            guint64 lost_frame_count = GST_BUFFER_OFFSET(*buf) - v4l2src->offset - 1;
            GST_WARNING("lost frames detected: count = %" G_GUINT64_FORMAT " - ts: %d"
            GST_TIME_FORMAT, lost_frame_count, GST_TIME_ARGS(timestamp));

            qos_msg = gst_message_new_qos(GST_OBJECT_CAST(v4l2src), TRUE,
                GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, timestamp,
                GST_CLOCK_TIME_IS_VALID(duration) ? lost_frame_count *
                duration : GST_CLOCK_TIME_NONE);
            gst_element_post_message(GST_ELEMENT_CAST(v4l2src), qos_msg);
        }
        v4l2src->offset = GST_BUFFER_OFFSET(*buf);
    }

    GST_DEBUG("timestamp: %" GST_TIME_FORMAT " duration: %" GST_TIME_FORMAT
      , GST_TIME_ARGS (timestamp), GST_TIME_ARGS (duration));

    GST_BUFFER_TIMESTAMP(*buf) = timestamp;
    GST_BUFFER_PTS (*buf) = timestamp;
    GST_BUFFER_DTS (*buf) = timestamp;
    GST_BUFFER_DURATION(*buf) = duration;

    return ret;
}

static gboolean
gst_sunxiv4l2src_start(GstBaseSrc *bsrc)
{
    GstSunxiV4l2Src *v4l2src = GST_SUNXI_V4L2SRC(bsrc);
    gint ret;
    gpointer v4l2handle;

    GST_DEBUG("open %s device io-mode %d", v4l2src->device, v4l2src->io_mode);

    v4l2handle = gst_sunxiv4l2_open_device(v4l2src->device, V4L2_CAP_VIDEO_CAPTURE_MPLANE);

    GST_OBJECT_LOCK(v4l2src);

    if (!v4l2handle) {
        GST_OBJECT_UNLOCK(v4l2src);
        return FALSE;
    }

    v4l2src->v4l2handle = v4l2handle;

    GST_OBJECT_UNLOCK(v4l2src);

    return TRUE;
}

static gboolean
gst_sunxiv4l2src_stop(GstBaseSrc *bsrc)
{
    GstSunxiV4l2Src *v4l2src = GST_SUNXI_V4L2SRC(bsrc);

    if (v4l2src->v4l2handle) {
        if (v4l2src->stream_on)
            gst_sunxi_v4l2_streamoff(v4l2src->v4l2handle);
            
        gst_sunxiv4l2_close_device(v4l2src->v4l2handle);
    }


    return TRUE;
}

static gboolean
gst_sunxiv4l2src_decie_allocation(GstBaseSrc *bsrc, GstQuery *query)
{
    GstSunxiV4l2Src *v4l2src = GST_SUNXI_V4L2SRC(bsrc);
    SUNXIV4l2AllocatorContext ctx;
    GstCaps *caps;
    GstBufferPool *pool;
    guint size, min, max;
    GstAllocator *allocator = NULL;
    GstAllocationParams params;
    GstStructure *config;
    gboolean update_pool, update_allocator;
    GstVideoInfo vinfo;
    const GstStructure *structure;

    if (v4l2src->pool) {
        gst_query_parse_allocation(query, &caps, NULL);
        gst_video_info_init(&vinfo);
        gst_sunxi_video_info_from_caps(&vinfo, caps);

        if (gst_query_get_n_allocation_pools(query) > 0) {
            gst_query_set_nth_allocation_pool(query, 0, v4l2src->pool, vinfo.size, v4l2src->actual_buf_cnt, v4l2src->actual_buf_cnt);
        } else {
            gst_query_add_allocation_pool(query, v4l2src->pool, vinfo.size, v4l2src->actual_buf_cnt, v4l2src->actual_buf_cnt);
        }
        return TRUE;
    }

    gst_query_parse_allocation(query, &caps, NULL);
    gst_video_info_init(&vinfo);
    gst_sunxi_video_info_from_caps(&vinfo, caps);

    if (gst_query_get_n_allocation_params(query) > 0) {
        gst_query_parse_nth_allocation_param(query, 0, &allocator, &params);
        update_allocator = TRUE;
    } else {
        allocator = NULL;
        gst_allocation_params_init(&params);
        update_allocator = FALSE;
    }

    if (gst_query_get_n_allocation_pools(query) > 0) {
        gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min, &max);
        size = MAX(size, vinfo.size);
        update_pool = TRUE;
    } else {
        pool = NULL;
        size = vinfo.size;
        min = max = 0;
        update_pool = FALSE;
    }

    if (allocator == NULL || 
        !GST_IS_ALLOCATOR_SUNXIV4L2(allocator)) {
        
        if (allocator) {
            GST_DEBUG("unref proposaled allocator.");
            gst_object_unref(allocator);
        }

        GST_INFO_OBJECT(v4l2src, "using v4l2 source allocator.");

        ctx.v4l2_handle = v4l2src->v4l2handle;
        ctx.user_data = (gpointer)v4l2src;
        ctx.callback = gst_sunxi_v4l2_callocator_cb;
        allocator = v4l2src->allocator = gst_sunxi_v4l2_allocator_new(&ctx);
        if (!v4l2src->allocator) {
            GST_ERROR("New v4l2 allocator failed.");
            return FALSE;
        }
    }

    if (pool == NULL  /*|| v4l2src->use_v4l2_memory == TRUE */) {
        GstVideoInfo info;
        if (pool) {
            gst_object_ref(pool);
            goto skip;
        }

        GST_DEBUG("no pool, making new pool");

        structure = gst_caps_get_structure(v4l2src->old_caps, 0);

        if (gst_sunxi_video_info_from_caps(&info, v4l2src->old_caps)) {
            GST_ERROR_OBJECT(v4l2src, "invalid caps");
            return FALSE;
        }

        size = GST_VIDEO_INFO_SIZE(&info);
        pool = gst_video_buffer_pool_new();
    }
skip:
    v4l2src->pool = pool;

    max = min = DEFAULT_FRAMES_IN_V4L2_CAPTURE;

    v4l2src->actual_buf_cnt = max;

    config = gst_buffer_pool_get_config(pool);

    if (!gst_buffer_pool_config_has_option(config, \
        GST_BUFFER_POOL_OPTION_VIDEO_META)) {
            gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    }

    GST_DEBUG("'config': size:%u, min:%d, max:%d", size, min, max);

    gst_buffer_pool_config_set_params(config, caps, size, min, max);
    gst_buffer_pool_config_set_allocator(config, allocator, &params);
    if (!gst_buffer_pool_set_config(pool, config)) {
        GST_ERROR("apply config failed.");
        return FALSE;
    }

    if (update_allocator)
        gst_query_set_nth_allocation_param(query, 0, allocator, &params);
    else
        gst_query_add_allocation_param(query, allocator, &params);

    return pool ? gst_buffer_pool_set_active(pool, TRUE) : TRUE;
}

static GstCaps *
gst_sunxiv4l2src_default_caps()
{
    GstCaps *caps = gst_caps_new_empty();
    GstStructure *structure = gst_structure_from_string(GST_VIDEO_CAPS_MAKE(DEFAULT_FORMAT), NULL);
    gst_caps_append_structure(caps, structure);

    return caps;
}

static GstCaps *
gst_sunxi_v4l2src_get_all_caps()
{
    GstCaps *caps = gst_sunxiv4l2_get_device_caps(V4L2_CAP_VIDEO_CAPTURE_MPLANE);

    if (!caps)
    {
        g_print("Can't get caps from capture devcie, use the default setting.\n");
        g_print("Perhaps haven't capture device.\n");
        caps = gst_sunxiv4l2src_default_caps();
    }

    return caps;
}

static void
gst_sunxiv4l2_install_properties(GObjectClass *klass)
{
    g_object_class_install_property(klass, PROP_DEVICE,
                                    g_param_spec_string("device", "Device", "captur device",
                                                        DEFAULT_DEVICE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(klass, PROP_IOMODE,
                                    g_param_spec_uint("io-mode", "io-mode", "capture device io mode",
                                                      1, 3, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(klass, PROP_CAMERA_INDEX,
                                    g_param_spec_int("index", "index", "capture video index",
                                                      0, 2, 0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_sunxi_v4l2src_class_init(GstSunxiV4l2SrcClass *klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    GstBaseSrcClass *gstbasesrc_class;
    GstPushSrcClass *gstpushsrc_class;

    gobject_class = (GObjectClass *)klass;
    gstelement_class = (GstElementClass *)klass;
    gstbasesrc_class = (GstBaseSrcClass *)klass;
    gstpushsrc_class = (GstPushSrcClass *)klass;

    gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_get_property);

    gst_sunxiv4l2_install_properties(gobject_class);

    gst_element_class_add_pad_template(gstelement_class,
                                       gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                                                            gst_sunxi_v4l2src_get_all_caps()));

    gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_set_caps);
    gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_get_caps);
    gstbasesrc_class->fixate = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_fixate);
    gstbasesrc_class->query = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_query);
    gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_start);
    gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_stop);
    gstbasesrc_class->decide_allocation = GST_DEBUG_FUNCPTR(gst_sunxiv4l2src_decie_allocation);

    gstpushsrc_class->create = GST_DEBUG_FUNCPTR(gst_sunxi_v4l2src_create);

    gst_element_class_set_static_metadata(gstelement_class,
                                          "Video for linux 2 source", "Source/Video",
                                          "SUNXI VIN Video for linux 2 stream",
                                          "chenlong. jeck.chen@dbappsecurity.com.cn");

}

static void
gst_sunxi_v4l2src_init(GstSunxiV4l2Src *src)
{
    GstVideoFormat gst_fmt;

    memset(&src->info, 0, sizeof(GstVideoInfo));

    src->device = g_strdup(DEFAULT_DEVICE);
    src->info.fps_d = DEFAULT_DENOMINATOR;
    src->info.fps_n = DEFAULT_NUMERATOR;
    src->info.width = DEFAULT_WIDTH;
    src->info.height = DEFAULT_HEIGHT;
    src->info.size = DEFAULT_SIZE;
    src->v4l2handle = NULL;
    src->io_mode = V4L2_MEMORY_MMAP;

    gst_fmt = gst_video_format_from_string(DEFAULT_FORMAT);

    src->v4l2fmt = gst_video_format_to_fourcc(gst_fmt);

    gst_base_src_set_format(GST_BASE_SRC(src), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(src), TRUE);

    g_print("======= SUNXI VIN V4L2SRC: %s build on %s %s. =======\n", (VERSION), __DATE__, __TIME__);
}

static gboolean
plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(sunxiv4l2src_debug, "sunxiv4l2src", 0, "SUNXI VIN V4L2 (video for linux 2) source");

    return gst_element_register(plugin, "sunxiv4l2src", GST_RANK_NONE, GST_TYPE_SUNXI_V4L2SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    sunxiv4l2src,
    "SUNXI VIN V4L2 plugin",
    plugin_init,
    VERSION,
    "LGPL",
    PACKAGE,
    ORIG)