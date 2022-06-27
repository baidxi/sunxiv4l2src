#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/allocators/gstfdmemory.h>

#ifdef __USE_ALLWINNER_ISP__
#include <AWIspApi.h>
#endif

#include "gstsunxiv4l2.h"

GST_DEBUG_CATEGORY_STATIC (sunxiv4l2_debug);
#define GST_CAT_DEFAULT sunxiv4l2_debug

typedef struct _SunxiV4l2camer_mem_block SunxiV4l2camera_mem_block;

struct _SunxiV4l2camer_mem_block {
    gboolean initialized;
    gboolean used;
    struct v4l2_buffer v4l2_buf;
    gpointer start[3];
    size_t len[3];
};

typedef struct _SunxiV4l2cameraHandle SunxiV4l2cameraHandle;
typedef struct _SUNXIV4l2Handle SUNXIV4l2Handle;
typedef gint (*camera_config)(SUNXIV4l2Handle *handle, guint v4l2fmt, guint w, guint h, guint fps_n, guint fps_d);
typedef gint (*camera_ctrl)(SUNXIV4l2Handle *handle);
typedef gint (*camera_fmt)(SUNXIV4l2Handle *handle, guint v4l2fmt, GstVideoInfo *info);
typedef gint (*camera_buf_ops)(SUNXIV4l2Handle *handle, struct v4l2_buffer *v4l2_buf, gint idx);

typedef struct _camera_ops {
    camera_config config;
    camera_ctrl streamon;
    camera_ctrl streamoff;
    camera_fmt set_fmt;
    camera_buf_ops qbuf;
    camera_buf_ops dqbuf;
}camera_ops;

struct _SunxiV4l2cameraHandle{
    guint v4l2_fmt;
    gchar card[32];
    gchar driver[32];
    gint type;
    gint sensor_type;
    gint ispId;
    guint version;
    GstVideoInfo info;
    gboolean streamon;
    gint  buffer_count;
    gint win_w;
    gint win_h;
    guint fmt;
    guint *support_format_table;
    guint memory_mode;
    camera_ops ops;
    guint nplanes;
    gint camera_index;
#ifdef __USE_ALLWINNER_ISP__
    AWIspApi *ispPort;
#endif
};

struct _SUNXIV4l2Handle{
    gchar *device;
    gint  type;
    int   v4l2_fd;
    gint  in_w;
    gint  in_h;
    gint  fps_d;
    gint  fps_n;
    gboolean streamon;
    gboolean is_interlace;
    SunxiV4l2cameraHandle camera;
};

typedef struct {
    const gchar *caps_str;
    guint v4l2fmt;
    GstVideoFormat gstfmt;
    guint bits_per_pixel;
    guint flags;
}SUNXIV4L2FmtMap;

typedef struct {
    const gchar *name;
    gboolean bg;
    const gchar *bg_fb_name;
}SUNXIV4L2DeviceMap;

static SUNXIV4L2DeviceMap g_device_maps[] = {
    {"/dev/video0", FALSE, "/dev/fb0"},
    {"/dev/video1", TRUE, "/dev/fb0"}
};

#define MAKE_COLORIMETRY(r,m,t,p) {  \
  GST_VIDEO_COLOR_RANGE ##r, GST_VIDEO_COLOR_MATRIX_ ##m, \
  GST_VIDEO_TRANSFER_ ##t, GST_VIDEO_COLOR_PRIMARIES_ ##p }

#define DEFAULT_YUV_SD  0
#define DEFAULT_YUV_HD  1
#define DEFAULT_RGB     2
#define DEFAULT_GRAY    3
#define DEFAULT_UNKNOWN 4
#define DEFAULT_YUV_UHD 5

static const GstVideoColorimetry default_color[] = {
  MAKE_COLORIMETRY (_16_235, BT601, BT709, SMPTE170M),
  MAKE_COLORIMETRY (_16_235, BT709, BT709, BT709),
  MAKE_COLORIMETRY (_0_255, RGB, SRGB, BT709),
  MAKE_COLORIMETRY (_0_255, BT601, UNKNOWN, UNKNOWN),
  MAKE_COLORIMETRY (_UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN),
  MAKE_COLORIMETRY (_16_235, BT2020, BT2020_12, BT2020),
};


static gint
check_allwinner_soc()
{
    return 0;
}

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define GST_VIDEO_FORMAT_YUV422P GST_VIDEO_FORMAT_Y212_LE
#elif __BYTE_ORDER == __BIG_ENDIAN
#define GST_VIDEO_FORMAT_YUV422P GST_VIDEO_FORMAT_Y212_BE
#else
#error("unknown byte order")
#endif

static SUNXIV4L2FmtMap g_sunxiv4l2fmt_maps[] = {
    {GST_VIDEO_CAPS_MAKE("I420"), GST_MAKE_FOURCC('I','4','2','0'), GST_VIDEO_FORMAT_I420, 16, 0 },    
    // {GST_VIDEO_CAPS_MAKE("YUY2"), GST_MAKE_FOURCC('Y', 'U', 'Y', '2'), GST_VIDEO_FORMAT_YUY2, 16, 0},
    {GST_VIDEO_CAPS_MAKE("YUY2"), GST_MAKE_FOURCC('Y', 'U', 'Y', '2'), GST_VIDEO_FORMAT_YUY2, 16, 0},
    {GST_VIDEO_CAPS_MAKE("YUYV"), V4L2_PIX_FMT_YUYV, GST_VIDEO_FORMAT_YUY2, 16, 0},
    {GST_VIDEO_CAPS_MAKE("NV12"), V4L2_PIX_FMT_NV12, GST_VIDEO_FORMAT_NV12, 12, 0},
    {GST_VIDEO_CAPS_MAKE("NV16"), V4L2_PIX_FMT_NV16, GST_VIDEO_FORMAT_NV16, 16, 0},
    {GST_VIDEO_CAPS_MAKE("NV21"), V4L2_PIX_FMT_NV21, GST_VIDEO_FORMAT_NV21, 24, 0},
    {GST_VIDEO_CAPS_MAKE("NV61"), V4L2_PIX_FMT_NV61, GST_VIDEO_FORMAT_NV61, 24, 0},
};

static SUNXIV4L2FmtMap *sunxi_v4l2_get_fmt_map(guint *map_size)
{
    SUNXIV4L2FmtMap *fmt_map = NULL;
    *map_size = 0;

    if (!check_allwinner_soc()) {
        fmt_map = g_sunxiv4l2fmt_maps;
        *map_size = ARRAY_SIZE(g_sunxiv4l2fmt_maps);
    } else {

    }

    return fmt_map;

}

static gint
gst_sunxi_v4l2_camera_config(SUNXIV4l2Handle *handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
    struct v4l2_format v4l2_fmt = {0};
    struct v4l2_frmsizeenum fszenum = {0};
    struct v4l2_streamparm parm = {0};

    gint capture_mode = -1;

    fszenum.index = 0;
    fszenum.pixel_format = fmt;

    while(ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &fszenum) >= 0) {
        if (fszenum.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            if (fszenum.stepwise.max_width == w && fszenum.stepwise.max_height == h) {
                capture_mode = fszenum.index;
                break;
            }
        } else {
            if (fszenum.discrete.width == w && fszenum.discrete.height == h) {
                capture_mode = fszenum.index;
                break;
            }
        }

        fszenum.index++;
    }

    if (capture_mode < 0) {
        GST_ERROR("can't support resolution.");
        return -1;
    }

    parm.type = handle->camera.type;
    parm.parm.capture.timeperframe.denominator = fps_d;
    parm.parm.capture.timeperframe.numerator = fps_n;
    parm.parm.capture.capturemode = capture_mode;

    if (ioctl(handle->v4l2_fd, VIDIOC_S_PARM, &parm) < 0) {
        GST_ERROR("VIDIOC_S_PARM failed.");
        return -1;
    }

    memset(&parm, 0, sizeof(parm));

    parm.type = handle->camera.type;

    if (ioctl(handle->v4l2_fd, VIDIOC_G_PARM, &parm) < 0) {
        GST_ERROR("Get %s parms failed.", handle->device);
        return -1;
    }

    handle->fps_d = fps_d;
    handle->fps_n = fps_n;

    GST_DEBUG("fps: %d/%d", parm.parm.capture.timeperframe.denominator, parm.parm.capture.timeperframe.numerator);

    return 0;
}

static gint
gst_sunxi_v4l2_camera_set_fmt(SUNXIV4l2Handle *handle, guint v4l2fmt, GstVideoInfo *info)
{
    struct v4l2_format fmt = {0};

    if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = info->width;
        fmt.fmt.pix_mp.height = info->height;
        fmt.fmt.pix_mp.pixelformat = v4l2fmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    } else {
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = info->width;
        fmt.fmt.pix.height = info->height;
        fmt.fmt.pix.pixelformat = v4l2fmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (ioctl(handle->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        GST_DEBUG("[%c%c%c%c]", v4l2fmt & 0xff, (v4l2fmt >> 8) & 0xff, 
                    (v4l2fmt >> 16) & 0xff, (v4l2fmt >> 24) & 0xff);
        GstVideoFormat gst_fmt = gst_video_format_from_fourcc(v4l2fmt);

        return -1;
    }

    if (ioctl(handle->v4l2_fd, VIDIOC_G_FMT, &fmt) < 0) {
        GST_ERROR("Getting format data failed errno:%d.", errno);
        return -1;
    }

    handle->camera.fmt = v4l2fmt;

    if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        handle->camera.win_w = fmt.fmt.pix_mp.width;
        handle->camera.win_h = fmt.fmt.pix_mp.height;
        handle->camera.nplanes = fmt.fmt.pix_mp.num_planes;
    } else {
        handle->camera.win_w = fmt.fmt.pix.width;
        handle->camera.win_h = fmt.fmt.pix.height;
    }

    GST_DEBUG("Format set to %c%c%c%c OK planes:%d", v4l2fmt & 0xff, (v4l2fmt >> 8) & 0xff, 
                    (v4l2fmt >> 16) & 0xff, (v4l2fmt >> 24) & 0xff, handle->camera.nplanes);

    return 0;

}

static gint
gst_sunxi_v4l2_camera_q_buffer(SUNXIV4l2Handle *handle, struct v4l2_buffer *v4l2_buf, gint idx)
{
    return ioctl(handle->v4l2_fd, VIDIOC_QBUF, v4l2_buf);
}

static gint
gst_sunxi_v4l2_camera_dq_buffer(SUNXIV4l2Handle *handle, struct v4l2_buffer *v4l2_buf, gint idx)
{
    gint ret;

    ret = ioctl(handle->v4l2_fd, VIDIOC_DQBUF, v4l2_buf);

    if (ret == 0)
        GST_DEBUG("**************DQBUF[%d] FINISH*****************************", idx);
    else {
        GST_ERROR("**************DQBUF[%d] FAILED*****************************", idx);
        return -1;
    }
    return 0;
}

static gint
gst_sunxi_v4l2_camera_streamon(SUNXIV4l2Handle *handle)
{
    enum v4l2_buf_type type;

    GST_DEBUG("STREAMON");

    if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(handle->v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        GST_ERROR("VIDIOC_STREAMON FAILED.");
        return -1;
    }

    handle->streamon = TRUE;

    GST_DEBUG("streamon OK");

    return 0;
}

static gint
gst_sunxi_v4l2_camera_streamoff(SUNXIV4l2Handle *handle)
{
    enum v4l2_buf_type type;

    handle->streamon = FALSE;

    if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (ioctl(handle->v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
        GST_ERROR("VIDIOC_STREAMOFF FAILED.");
        return -1;
    }

    return 0;
}

static gint
gst_sunxiv4l2capture_set_function(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    handle->camera.ops.config = gst_sunxi_v4l2_camera_config;
    handle->camera.ops.streamon = gst_sunxi_v4l2_camera_streamon;
    handle->camera.ops.streamoff = gst_sunxi_v4l2_camera_streamoff;
    handle->camera.ops.set_fmt = gst_sunxi_v4l2_camera_set_fmt;
    handle->camera.ops.dqbuf = gst_sunxi_v4l2_camera_dq_buffer;
    handle->camera.ops.qbuf = gst_sunxi_v4l2_camera_q_buffer;

    return 0;
}

static gint
gst_sunxi_v4l2_get_sensor_type(int fd)
{
    gint ret = -1;
    struct v4l2_control ctrl = {0};
    struct v4l2_queryctrl qc_ctrl = {0};

    ctrl.id = V4L2_CID_SENSOR_TYPE;
    qc_ctrl.id = V4L2_CID_SENSOR_TYPE;

    if (-1 == ioctl(fd, VIDIOC_QUERYCTRL, &qc_ctrl)) {
        GST_ERROR("query sensor type ctrl failed. errno(%d)", errno);
        return -1;
    }

    ret = ioctl(fd, VIDIOC_G_CTRL, &ctrl);

    if (ret < 0) {
        GST_ERROR("Get sensor type failed. errno(%d)", errno);
        return ret;
    }

    return ctrl.value;
}

gpointer gst_sunxiv4l2_open_device(gchar *device, int type)
{
    int fd;
    struct v4l2_capability cap;
    struct v4l2_input inp;
    gint sensor_type;
    size_t len;
    SUNXIV4l2Handle *handle = NULL;

    GST_DEBUG_CATEGORY_INIT(sunxiv4l2_debug, "sunxiv4l2", 0, "SUNXI VIN V4L2 Core");

    GST_INFO("device name: %s", device);

    fd = open(device, O_RDWR, 0);

    if (fd < 0) {
        GST_DEBUG("Can't open %s.\n", device);
        return NULL;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        GST_ERROR("VIDIOC_QUERYCAP error.");
        close(fd);
        return NULL;
    }

    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 && (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0) {
        GST_DEBUG("device can't capture.");
        close(fd);
        return NULL;
    }

#ifdef __USE_ALLWINNER_ISP__
    GST_DEBUG("Set input device");
    memset(&inp, 0, sizeof(inp));
    inp.index = 0;
    inp.type = V4L2_INPUT_TYPE_CAMERA;

    if (ioctl(fd, VIDIOC_S_INPUT, &inp) < 0) {
        GST_ERROR("VIDIOC_S_OUTPUT failed.", inp.index);
        close(fd);
        return NULL;            
    }

    if (strcmp(cap.card, "sunxi-vin") == 0) {
        sensor_type = gst_sunxi_v4l2_get_sensor_type(fd);
        if (sensor_type < 0) {
            GST_ERROR("Get sensor type FAILED.");
            return NULL;
        }
        GST_DEBUG("sensor type:%d", sensor_type);
    }

#endif
    handle = (SUNXIV4l2Handle *)g_slice_alloc(sizeof(SUNXIV4l2Handle));
    if (!handle) {
        GST_ERROR("allocate for SUNXIV4l2Hanle failed.\n");
        close(fd);
        return NULL;
    }

    memset(handle, 0, sizeof(SUNXIV4l2Handle));

    GST_DEBUG("driver:%s,card:%s,version:0x%x", cap.driver, cap.card, cap.version);

    handle->v4l2_fd = fd;
    handle->device = device;
    handle->streamon = FALSE;
    handle->type = type;

#ifdef __USE_ALLWINNER_ISP__
    handle->camera.sensor_type = sensor_type;

    if (sensor_type == V4L2_SENSOR_TYPE_RAW) {
        handle->camera.ispPort = CreateAWIspApi();
        GST_DEBUG("raw sensor use vin isp");
    }
#endif
    len = strlen(cap.driver);
    if (len && len < 32)
        strncpy((char *)&handle->camera.driver, cap.driver, len);

    len = strlen(cap.card);
    if (len && len < 32)
        strncpy((char *)&handle->camera.card, cap.card, len);

    handle->camera.version = cap.version;

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        handle->camera.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        handle->camera.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (type & V4L2_CAP_VIDEO_CAPTURE_MPLANE || type & V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        if (gst_sunxiv4l2capture_set_function(handle) < 0) {
            GST_ERROR("v4l2 capture set function failed.\n");
            close(fd);
            g_slice_free1(sizeof(SUNXIV4l2Handle), handle);
            return NULL;
        }
    }

    return (gpointer)handle;
}

gint
gst_sunxiv4l2_close_device(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    if (handle) {
#ifdef __USE_ALLWINNER_ISP__
        if (strcmp(handle->camera.card, "sunxi-vin") == 0) {
            if (handle->camera.sensor_type == V4L2_SENSOR_TYPE_RAW) {
                if (handle->camera.ispId >= 0) {
                    handle->camera.ispPort->ispStop(handle->camera.ispId);
                }

                DestroyAWIspApi(handle->camera.ispPort);
                handle->camera.ispPort = NULL;
            }
        }
#endif
        if (handle->v4l2_fd) {
            GST_DEBUG("close %s V4L2 device", handle->device);
            close(handle->v4l2_fd);
            handle->v4l2_fd = 0;
        }
        g_slice_free1(sizeof(SUNXIV4l2Handle), handle);
    }

    return 0;
}

GstCaps *
gst_sunxiv4l2_get_caps(gpointer v4l2handle)
{
    struct v4l2_fmtdesc fmtdesc = {0};
    struct v4l2_frmsizeenum frmsize = {0};
    struct v4l2_frmivalenum frmival = {0};
    gint i, index, vformat;
    GstCaps *caps = NULL;
    SUNXIV4l2Handle *handle = (SUNXIV4l2Handle *)v4l2handle;

    if (handle->type & V4L2_CAP_VIDEO_CAPTURE || handle->type & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        fmtdesc.index = 0;
        fmtdesc.type = handle->camera.type;

        if (handle->camera.support_format_table) {
            while(handle->camera.support_format_table[fmtdesc.index]) {
                fmtdesc.pixelformat = handle->camera.support_format_table[fmtdesc.index];
                vformat = fmtdesc.pixelformat;
                GST_INFO("frame format: %c%c%c%c",
                    vformat & 0xff, (vformat >> 8) & 0xff,
                    (vformat >> 16) & 0xff, (vformat >> 24) & 0xff);
                frmsize.pixel_format = fmtdesc.pixelformat;
                frmsize.index = 0;
                while(ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0) {
                    GST_INFO("frame size: %dx%d", frmsize.discrete.width, frmsize.discrete.height);
                    GST_INFO("frame size type: %d", frmsize.type);
                    if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        frmival.index = 0;
                        frmival.pixel_format = fmtdesc.pixelformat;
                        frmival.width = frmsize.discrete.width;
                        frmival.height = frmsize.discrete.height;
                        while(ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) >= 0) {
                            GST_INFO("frame rate: %d/%d", frmival.discrete.denominator, frmival.discrete.numerator);
                            index = 0;
                            while(handle->camera.support_format_table[index]) {
                                guint map_size;
                                SUNXIV4L2FmtMap *fmt_map = sunxi_v4l2_get_fmt_map(&map_size);

                                for (i = 0; i <= map_size; i++) {
                                    if (handle->camera.support_format_table[index] == fmt_map[i].v4l2fmt) {
                                        if (!caps)
                                            caps = gst_caps_new_empty();

                                        if (caps) {
                                            GstStructure *structure = gst_structure_from_string( \
                                                fmt_map[i].caps_str, NULL);
                                            gst_structure_set(structure, "width", G_TYPE_INT, frmsize.discrete.width, NULL);
                                            gst_structure_set(structure, "height", G_TYPE_INT, frmsize.discrete.height, NULL);
                                            gst_structure_set(structure, "framerate", GST_TYPE_FRACTION, \
                                                frmival.discrete.denominator, frmival.discrete.numerator, NULL);
                                            if (handle->is_interlace)
                                                gst_structure_set(structure, "interlace-mode", G_TYPE_STRING, "interleaved", NULL);
                                            gst_caps_append_structure(caps, structure);
                                            GST_INFO("Added one caps\n");
                                        }
                                    }
                                }
                                index++;
                            }
                            frmival.index++;
                        }
                    }
                    frmsize.index++;
                }
                fmtdesc.index++;
            }
        } else {
            while(ioctl(handle->v4l2_fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0) {
                vformat = fmtdesc.pixelformat;
                GST_INFO("frame format: %c%c%c%c", 
                    vformat & 0xff, (vformat >> 8) & 0xff, 
                    (vformat >> 16) & 0xff, (vformat >> 24) & 0xff
                );
                frmsize.pixel_format = fmtdesc.pixelformat;
                frmsize.index = 0;

                while(ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0) {

                    frmival.index = 0;
                    frmival.pixel_format = fmtdesc.pixelformat;

                    if (frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                        frmival.width = frmsize.stepwise.max_width;
                        frmival.height = frmsize.stepwise.max_height;
                    } else {
                        frmival.width = frmsize.discrete.width;
                        frmival.height = frmsize.discrete.height;
                    }

                    GST_INFO("frame size: %dx%d ", frmival.width, frmival.height);
                    GST_INFO("frame size type: %d ", frmsize.type);

try_enumframesize:
                    if (ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) < 0) {
                        guint map_size;
                        SUNXIV4L2FmtMap *fmt_map = sunxi_v4l2_get_fmt_map(&map_size);          

                        for (i = 0; i <= map_size; i++) {
                            if (fmtdesc.pixelformat == fmt_map[i].v4l2fmt) {
                                if (!caps)
                                    caps = gst_caps_new_empty();
                                if (caps) {
                                    GstStructure *structure = gst_structure_from_string(fmt_map[i].caps_str, NULL);
                                    gst_structure_set(structure, "width", G_TYPE_INT, frmsize.type == V4L2_FRMIVAL_TYPE_DISCRETE ? frmsize.discrete.width : frmsize.stepwise.max_width, NULL);
                                    gst_structure_set(structure, "height", G_TYPE_INT, frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ? frmsize.discrete.height : frmsize.stepwise.max_height, NULL);
                                    gst_structure_set(structure, "framerate", GST_TYPE_FRACTION, handle->fps_d ? handle->fps_d : 30, handle->fps_n ? handle->fps_n : 1 , NULL);
                                    gst_caps_append_structure(caps, structure);
                                    GST_INFO("Added one caps\n");
                                }
                            }                            
                        }              
                    } else {
                        GST_INFO("frame rate: %d/%d", frmival.discrete.denominator, frmival.discrete.numerator);
                        guint map_size;
                        SUNXIV4L2FmtMap *fmt_map = sunxi_v4l2_get_fmt_map(&map_size);

                        for (i = 0; i <= map_size; i++) {                              
                            if (fmtdesc.pixelformat == fmt_map[i].v4l2fmt) {
                                if (!caps)
                                    caps = gst_caps_new_empty();
                                if (caps) {
                                    GstStructure *structure = gst_structure_from_string( \
                                        fmt_map[i].caps_str, NULL);
                                    gst_structure_set(structure, "width", G_TYPE_INT, frmsize.discrete.width, NULL);
                                    gst_structure_set(structure, "height", G_TYPE_INT, frmsize.discrete.height, NULL);
                                    gst_structure_set(structure, "framerate", GST_TYPE_FRACTION, \
                                        frmival.discrete.denominator, frmival.discrete.numerator, NULL);
                                    gst_caps_append_structure(caps, structure);
                                    GST_INFO("Added one caps\n");
                                }
                            }
                        }                        
                        frmival.index++; 
                        goto try_enumframesize;                    
                    }
                    frmsize.index++;
                }
                fmtdesc.index++;
            }
        }
    }

    return caps;
}

static GstCaps *
gst_sunxiv4l2capture_get_device_caps()
{
#define MAX_DEVICE  2
    gint i;
    int fd;
    GstCaps *caps = NULL;
    gpointer v4l2handle;
    gchar devname[32] = {0};

    for (i = 0; i < MAX_DEVICE; i++) {
        sprintf(devname, "/dev/video%d", i);

        v4l2handle = gst_sunxiv4l2_open_device(devname,  V4L2_CAP_VIDEO_CAPTURE_MPLANE);

        if (v4l2handle) {
            if (!caps)
                caps = gst_caps_new_empty();

            if (caps) {
                GstCaps *dev_caps = gst_sunxiv4l2_get_caps(v4l2handle);
                if (dev_caps)
                    gst_caps_append(caps, dev_caps);
            }
            gst_sunxiv4l2_close_device(v4l2handle);
            v4l2handle = NULL;
        }
    }
    return caps;
}

GstCaps *gst_sunxiv4l2_get_device_caps(gint type)
{
    struct v4l2_fmtdesc fmtdesc;
    char *devname;
    int fd;
    gint i;
    GstCaps *caps = NULL;

    if (type & V4L2_CAP_VIDEO_CAPTURE || type & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        return gst_sunxiv4l2capture_get_device_caps();
    }

    return caps;
}

gint
gst_sunxi_v4l2_set_buffer_count(gpointer v4l2handle, guint count, guint memory_mode)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    struct v4l2_requestbuffers buf_req;

    GST_DEBUG("request for (%d) buffers.camera type:0x%x", count, handle->camera.type);

    memset(&buf_req, 0, sizeof(buf_req));

    buf_req.type = handle->camera.type;
    buf_req.count = count;
    buf_req.memory = handle->camera.memory_mode = memory_mode;

    if (ioctl(handle->v4l2_fd, VIDIOC_REQBUFS, &buf_req) < 0) {
        GST_ERROR("Request %d buffer failed(%d).", count, errno);
        return -1;
    }

    handle->camera.buffer_count = buf_req.count;

    return 0;
    
}

gint
gst_sunxi_v4l2_free_buffer(gpointer v4l2handle, gint idx)
{
    return 0;
}

gint
gst_sunxi_v4l2_allocate_buffer(gpointer v4l2handle, gint idx, struct v4l2_buffer *v4l2_buf)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    gint i;
    GstMemory *mem;

    g_return_val_if_fail(v4l2_buf != NULL, -1);

    GST_DEBUG("idx:%d, memory mode:%d", idx, handle->camera.memory_mode);

    if (strcmp(handle->camera.card, "sunxi-vin") == 0) {
        if (handle->camera.memory_mode == V4L2_MEMORY_USERPTR) {
            GST_INFO("USERPTR mode, needn't allocator memory.\n");
            return 0;
        }

        v4l2_buf->type = handle->camera.type;
        v4l2_buf->memory = handle->camera.memory_mode;
        v4l2_buf->index = idx;

        if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            v4l2_buf->length = handle->camera.nplanes;
            v4l2_buf->m.planes = (struct v4l2_plane *)calloc(handle->camera.nplanes, sizeof(struct v4l2_plane));
            if (!v4l2_buf->m.planes) {
                GST_ERROR("allocate v4l2 plane failed (%d)", idx);
                return -1;
            }
        }

        if (ioctl(handle->v4l2_fd, VIDIOC_QUERYBUF, v4l2_buf) < 0) {
            GST_ERROR("VIDIOC_QUERYBUF FAIL.");
            if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
                free(v4l2_buf->m.planes);
            }

            return -1;
        }
        
        return handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE ? handle->camera.nplanes : 0;
    } else {

    }

    return 0;
    
}

GstFlowReturn
gst_sunxi_v4l2_memory_map_full(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gpointer *data, GstMapFlags flags)
{
    gint i;
    SUNXIV4l2Handle *handle = v4l2handle;
    SunxiV4l2camera_mem_block *blk;
    GST_DEBUG("nplanes:%d, flags:0x%x, data:%p", handle->camera.nplanes, flags, data);
    
    g_return_val_if_fail(v4l2_buf != NULL, GST_FLOW_ERROR);
    g_return_val_if_fail(data != NULL, GST_FLOW_ERROR);
    
    blk = g_slice_alloc0(sizeof(SunxiV4l2camera_mem_block));

    g_return_val_if_fail(blk != NULL, GST_FLOW_ERROR);

    if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        for (i = 0; i < handle->camera.nplanes; i++) {
            blk->len[i] = v4l2_buf->m.planes[i].length;
            blk->start[i] = mmap(NULL,
                                v4l2_buf->m.planes[i].length,
                                flags, MAP_SHARED,
                                handle->v4l2_fd, v4l2_buf->m.planes[i].m.mem_offset);

            if (blk->start[i] == MAP_FAILED) {
                GST_ERROR("map FAILED.");
                g_slice_free1(sizeof(SunxiV4l2camera_mem_block), blk);
                return GST_FLOW_ERROR;
            }
        }
        free(v4l2_buf->m.planes);
        v4l2_buf->m.planes = NULL;
    } else {
        blk->len[0] = v4l2_buf->length;
        blk->start[0] = mmap(NULL, v4l2_buf->length, flags, MAP_SHARED, handle->v4l2_fd, v4l2_buf->m.offset);
        if (blk->start[0] == MAP_FAILED) {
            GST_ERROR("map FAILED.");
            g_slice_free1(sizeof(SunxiV4l2camera_mem_block), blk);
            return GST_FLOW_ERROR;
        }
    }

    blk->initialized = TRUE;

    *data = blk;

    return GST_FLOW_OK;

}

void
gst_sunxi_v4l2_memory_unmap(gpointer v4l2handle, gint idx, struct v4l2_buffer *buf)
{
    
}

guint
gst_sunxiv_v4l2_fmt_gst2v4l2(GstVideoFormat gstfmt)
{
    guint v4l2_fmt = 0;
    int i;
    guint map_size;

    SUNXIV4L2FmtMap *fmt_map = sunxi_v4l2_get_fmt_map(&map_size);

    for (i = 0; i < map_size; i++) {
        if (gstfmt == fmt_map[i].gstfmt) {
            v4l2_fmt = fmt_map[i].v4l2fmt;
            break;
        }
    }

    return v4l2_fmt;
}

gint
gst_sunxi_v4l2capture_config(gpointer v4l2handle, guint v4l2fmt, guint w, guint h, guint fps_n, guint fps_d)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->camera.ops.config(handle, v4l2fmt, w, h, fps_n, fps_d);
}

static gint isOutputRawData(gint v4l2_fmt) {
    if (v4l2_fmt == V4L2_PIX_FMT_SBGGR8 ||
        v4l2_fmt == V4L2_PIX_FMT_SGBRG8 ||
        v4l2_fmt == V4L2_PIX_FMT_SGRBG8 ||
        v4l2_fmt == V4L2_PIX_FMT_SRGGB8 ||
        
        v4l2_fmt == V4L2_PIX_FMT_SBGGR10 ||
        v4l2_fmt == V4L2_PIX_FMT_SGBRG10 ||
        v4l2_fmt == V4L2_PIX_FMT_SGRBG10 ||
        v4l2_fmt == V4L2_PIX_FMT_SRGGB10 ||
        
        v4l2_fmt == V4L2_PIX_FMT_SBGGR12 ||
        v4l2_fmt == V4L2_PIX_FMT_SGBRG12 ||
        v4l2_fmt == V4L2_PIX_FMT_SGRBG12 ||
        v4l2_fmt == V4L2_PIX_FMT_SRGGB12)
        return 1;
    else
        return 0;
}

gboolean
gst_sunxi_v4l2_streamon(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    if (handle->camera.ops.streamon(handle)) {
        GST_ERROR("STREAMON FAILED.");
        return FALSE;
    }
#ifdef __USE_ALLWINNER_ISP__
    if (handle->camera.sensor_type == V4L2_SENSOR_TYPE_RAW) {
        GST_DEBUG("sensor type:%d", handle->camera.sensor_type);
        if (!isOutputRawData(handle->camera.v4l2_fmt)) {
            handle->camera.ispId = -1;
            handle->camera.ispId = handle->camera.ispPort->ispGetIspId(handle->camera.camera_index);
            if (handle->camera.ispId >= 0) {
                GST_DEBUG("START ISP");
                handle->camera.ispPort->ispStart(handle->camera.ispId);
            }
        }
    }
#endif

    return TRUE;
}

gint
gst_sunxi_v4l2_streamoff(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    if (handle->camera.streamon)
        return handle->camera.ops.streamoff(handle);
    
    return 0;
}

gint
gst_sunxi_v4l2_set_format(gpointer v4l2handle, guint v4l2fmt, GstVideoInfo *info)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->camera.ops.set_fmt(handle, v4l2fmt, info);
}

static GstVideoFormat gst_sunxi_format_from_string(const gchar *str)
{
    guint v4l2fmt;
    size_t size = strlen(str);
    guint map_size;
    gint i;
    SUNXIV4L2FmtMap *fmt_map;
    GstVideoFormat fmt = GST_VIDEO_FORMAT_UNKNOWN;

    if (size > 4)
        return fmt;

    v4l2fmt = str[0] | str[1] << 8 | str[2] << 16 | str[3] << 24;

    fmt_map = sunxi_v4l2_get_fmt_map(&map_size);

    for (i = 0; i < map_size; i++) {
        if (fmt_map[i].v4l2fmt == v4l2fmt) {
            fmt = fmt_map[i].gstfmt;
            break;
        }
    }

    return fmt;
}

static gboolean
fill_planes (GstVideoInfo * info)
{
  gsize width, height, cr_h;
  gint bpp = 0, i;

  width = (gsize) info->width;
  height = (gsize) GST_VIDEO_INFO_FIELD_HEIGHT (info);

  /* Sanity check the resulting frame size for overflows */
  for (i = 0; i < GST_VIDEO_INFO_N_COMPONENTS (info); i++)
    bpp += GST_VIDEO_INFO_COMP_DEPTH (info, i);
  bpp = GST_ROUND_UP_8 (bpp) / 8;
  if (bpp > 0 && GST_ROUND_UP_128 ((guint64) width) * ((guint64) height) >=
      G_MAXUINT / bpp) {
    GST_ERROR ("Frame size %ux%u would overflow", info->width, info->height);
    return FALSE;
  }

  switch (info->finfo->format) {
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_YVYU:
    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_VYUY:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->offset[0] = 0; 
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_AYUV:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_r210:
    case GST_VIDEO_FORMAT_Y410:
    case GST_VIDEO_FORMAT_VUYA:
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      info->stride[0] = width * 4;
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_RGB16:
    case GST_VIDEO_FORMAT_BGR16:
    case GST_VIDEO_FORMAT_RGB15:
    case GST_VIDEO_FORMAT_BGR15:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_v308:
    case GST_VIDEO_FORMAT_IYU2:
      info->stride[0] = GST_ROUND_UP_4 (width * 3);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_v210:
      info->stride[0] = ((width + 47) / 48) * 128;
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_v216:
    case GST_VIDEO_FORMAT_Y210:
      info->stride[0] = GST_ROUND_UP_8 (width * 4);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_GRAY8:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_GRAY16_BE:
    case GST_VIDEO_FORMAT_GRAY16_LE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_UYVP:
      info->stride[0] = GST_ROUND_UP_4 ((width * 2 * 5 + 3) / 4);
      info->offset[0] = 0;
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_RGB8P:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = 4;
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->size = info->offset[1] + (4 * 256);
      break;
    case GST_VIDEO_FORMAT_IYU1:
      info->stride[0] = GST_ROUND_UP_4 (GST_ROUND_UP_4 (width) +
          GST_ROUND_UP_4 (width) / 2);
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_ARGB64:
    case GST_VIDEO_FORMAT_AYUV64:
      info->stride[0] = width * 8;
      info->offset[0] = 0;
      info->size = info->stride[0] * height;
      break;
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:        /* same as I420, but plane 1+2 swapped */
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_4 (GST_ROUND_UP_2 (width) / 2);
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->offset[2] = info->offset[1] + info->stride[1] * cr_h;
      info->size = info->offset[2] + info->stride[2] * cr_h;
      break;
    case GST_VIDEO_FORMAT_Y41B:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_16 (width) / 4;
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] + info->stride[1] * height;
      /* simplification of ROUNDUP4(w)*h + 2*((ROUNDUP16(w)/4)*h */
      info->size = (info->stride[0] + (GST_ROUND_UP_16 (width) / 2)) * height;
      break;
    case GST_VIDEO_FORMAT_Y42B:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_8 (width) / 2;
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] + info->stride[1] * height;
      /* simplification of ROUNDUP4(w)*h + 2*(ROUNDUP8(w)/2)*h */
      info->size = (info->stride[0] + GST_ROUND_UP_8 (width)) * height;
      break;
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_GBR:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = info->stride[0];
      info->stride[2] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] * 2;
      info->size = info->stride[0] * height * 3;
      break;
    case GST_VIDEO_FORMAT_GBRA:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = info->stride[0];
      info->stride[2] = info->stride[0];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] * 2;
      info->offset[3] = info->offset[1] * 3;
      info->size = info->stride[0] * height * 4;
      break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->size = info->offset[1] + info->stride[0] * cr_h;
      break;
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_NV61:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->size = info->stride[0] * height * 2;
      break;
    case GST_VIDEO_FORMAT_NV24:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_4 (width * 2);
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->size = info->stride[0] * height + info->stride[1] * height;
      break;
    case GST_VIDEO_FORMAT_A420:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_4 (GST_ROUND_UP_2 (width) / 2);
      info->stride[2] = info->stride[1];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->offset[2] = info->offset[1] + info->stride[1] * cr_h;
      info->offset[3] = info->offset[2] + info->stride[2] * cr_h;
      info->size = info->offset[3] + info->stride[0] * GST_ROUND_UP_2 (height);
      break;
    case GST_VIDEO_FORMAT_YUV9:
    case GST_VIDEO_FORMAT_YVU9:
      info->stride[0] = GST_ROUND_UP_4 (width);
      info->stride[1] = GST_ROUND_UP_4 (GST_ROUND_UP_4 (width) / 4);
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      cr_h = GST_ROUND_UP_4 (height) / 4;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->offset[2] = info->offset[1] + info->stride[1] * cr_h;
      info->size = info->offset[2] + info->stride[2] * cr_h;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
    case GST_VIDEO_FORMAT_I420_10BE:
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_I420_12BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = GST_ROUND_UP_4 (width);
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->offset[2] = info->offset[1] + info->stride[1] * cr_h;
      info->size = info->offset[2] + info->stride[2] * cr_h;
      break;
    case GST_VIDEO_FORMAT_I422_10LE:
    case GST_VIDEO_FORMAT_I422_10BE:
    case GST_VIDEO_FORMAT_I422_12LE:
    case GST_VIDEO_FORMAT_I422_12BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = GST_ROUND_UP_4 (width);
      info->stride[2] = info->stride[1];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      info->offset[2] = info->offset[1] +
          info->stride[1] * GST_ROUND_UP_2 (height);
      info->size = info->offset[2] + info->stride[2] * GST_ROUND_UP_2 (height);
      break;
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_Y444_10BE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_Y444_12BE:
    case GST_VIDEO_FORMAT_GBR_10LE:
    case GST_VIDEO_FORMAT_GBR_10BE:
    case GST_VIDEO_FORMAT_GBR_12LE:
    case GST_VIDEO_FORMAT_GBR_12BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = info->stride[0];
      info->stride[2] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] * 2;
      info->size = info->stride[0] * height * 3;
      break;
    case GST_VIDEO_FORMAT_GBRA_10LE:
    case GST_VIDEO_FORMAT_GBRA_10BE:
    case GST_VIDEO_FORMAT_GBRA_12LE:
    case GST_VIDEO_FORMAT_GBRA_12BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = info->stride[0];
      info->stride[2] = info->stride[0];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] * 2;
      info->offset[3] = info->offset[1] * 3;
      info->size = info->stride[0] * height * 4;
      break;
    case GST_VIDEO_FORMAT_NV12_64Z32:
      info->stride[0] =
          GST_VIDEO_TILE_MAKE_STRIDE (GST_ROUND_UP_128 (width) / 64,
          GST_ROUND_UP_32 (height) / 32);
      info->stride[1] =
          GST_VIDEO_TILE_MAKE_STRIDE (GST_ROUND_UP_128 (width) / 64,
          GST_ROUND_UP_64 (height) / 64);
      info->offset[0] = 0;
      info->offset[1] = GST_ROUND_UP_128 (width) * GST_ROUND_UP_32 (height);
      info->size = info->offset[1] +
          GST_ROUND_UP_128 (width) * GST_ROUND_UP_64 (height) / 2;
      break;
    case GST_VIDEO_FORMAT_A420_10LE:
    case GST_VIDEO_FORMAT_A420_10BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = GST_ROUND_UP_4 (width);
      info->stride[2] = info->stride[1];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->offset[2] = info->offset[1] + info->stride[1] * cr_h;
      info->offset[3] = info->offset[2] + info->stride[2] * cr_h;
      info->size = info->offset[3] + info->stride[0] * GST_ROUND_UP_2 (height);
      break;
    case GST_VIDEO_FORMAT_A422_10LE:
    case GST_VIDEO_FORMAT_A422_10BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = GST_ROUND_UP_4 (width);
      info->stride[2] = info->stride[1];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      info->offset[2] = info->offset[1] +
          info->stride[1] * GST_ROUND_UP_2 (height);
      info->offset[3] =
          info->offset[2] + info->stride[2] * GST_ROUND_UP_2 (height);
      info->size = info->offset[3] + info->stride[0] * GST_ROUND_UP_2 (height);
      break;
    case GST_VIDEO_FORMAT_A444_10LE:
    case GST_VIDEO_FORMAT_A444_10BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = info->stride[0];
      info->stride[1] = info->stride[0];
      info->stride[2] = info->stride[0];
      info->stride[3] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->offset[2] = info->offset[1] * 2;
      info->offset[3] = info->offset[1] * 3;
      info->size = info->stride[0] * height * 4;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P010_10BE:
      info->stride[0] = GST_ROUND_UP_4 (width * 2);
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      info->size = info->offset[1] + info->stride[0] * cr_h;
      break;
    case GST_VIDEO_FORMAT_GRAY10_LE32:
      info->stride[0] = (width + 2) / 3 * 4;
      info->offset[0] = 0;
      info->size = info->stride[0] * GST_ROUND_UP_2 (height);
      break;
    case GST_VIDEO_FORMAT_NV12_10LE32:
      info->stride[0] = (width + 2) / 3 * 4;
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->size = info->offset[1] + info->stride[0] * cr_h;
      break;
    case GST_VIDEO_FORMAT_NV16_10LE32:
      info->stride[0] = (width + 2) / 3 * 4;
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * height;
      info->size = info->stride[0] * height * 2;
      break;
    case GST_VIDEO_FORMAT_NV12_10LE40:
      info->stride[0] = ((width * 5 >> 2) + 4) / 5 * 5;
      info->stride[1] = info->stride[0];
      info->offset[0] = 0;
      info->offset[1] = info->stride[0] * GST_ROUND_UP_2 (height);
      cr_h = GST_ROUND_UP_2 (height) / 2;
      cr_h = GST_ROUND_UP_2 (height) / 2;
      if (GST_VIDEO_INFO_IS_INTERLACED (info))
        cr_h = GST_ROUND_UP_2 (cr_h);
      info->size = info->offset[1] + info->stride[0] * cr_h;
      break;

    case GST_VIDEO_FORMAT_ENCODED:
      break;
    case GST_VIDEO_FORMAT_UNKNOWN:
      GST_ERROR ("invalid format");
      g_warning ("invalid format");
      return FALSE;
      break;
  }

  return TRUE;
}
                                                                                                                                                                                            
static void
set_default_colorimetry (GstVideoInfo * info)
{
  const GstVideoFormatInfo *finfo = info->finfo;

  if (GST_VIDEO_FORMAT_INFO_IS_YUV (finfo)) {
    if (info->height >= 2160) {
      info->chroma_site = GST_VIDEO_CHROMA_SITE_H_COSITED;
      info->colorimetry = default_color[DEFAULT_YUV_UHD];
    } else if (info->height > 576) {
      info->chroma_site = GST_VIDEO_CHROMA_SITE_H_COSITED;
      info->colorimetry = default_color[DEFAULT_YUV_HD];
    } else {
      info->chroma_site = GST_VIDEO_CHROMA_SITE_NONE;
      info->colorimetry = default_color[DEFAULT_YUV_SD];
    }
  } else if (GST_VIDEO_FORMAT_INFO_IS_GRAY (finfo)) {
    info->colorimetry = default_color[DEFAULT_GRAY];
  } else if (GST_VIDEO_FORMAT_INFO_IS_RGB (finfo)) {
    info->colorimetry = default_color[DEFAULT_RGB];
  } else {
    info->colorimetry = default_color[DEFAULT_UNKNOWN];
  }
}

static gboolean
validate_colorimetry (GstVideoInfo * info)
{
  const GstVideoFormatInfo *finfo = info->finfo;

  if (!GST_VIDEO_FORMAT_INFO_IS_RGB (finfo) &&
      info->colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB) {
    GST_WARNING
        ("color matrix RGB is only supported with RGB format, %s is not",
        finfo->name);
    return FALSE;
  }

  if (GST_VIDEO_FORMAT_INFO_IS_YUV (finfo) &&
      info->colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
    GST_WARNING ("Need to specify a color matrix when using YUV format (%s)",
        finfo->name);
    return FALSE;
  }

  return TRUE;
}


gint gst_sunxi_video_info_from_caps(GstVideoInfo *info, GstCaps *caps)
{
    GstStructure *structure;
    const gchar *s;
    GstVideoFormat fmt = GST_VIDEO_FORMAT_UNKNOWN;
    gint width, height;
    gint fps_n, fps_d, par_n, par_d;
    guint multiview_flags;

    g_return_val_if_fail(info != NULL, -1);
    g_return_val_if_fail(caps != NULL, -1);
    g_return_val_if_fail(gst_caps_is_fixed(caps), -1);

    GST_DEBUG("parsing caps %"GST_PTR_FORMAT, caps);

    structure = gst_caps_get_structure(caps, 0);

    if (gst_structure_has_name(structure, "video/x-raw")) {
        if (!(s = gst_structure_get_string(structure, "format")))
            goto no_format;

        fmt = gst_sunxi_format_from_string(s);
        if (fmt == GST_VIDEO_FORMAT_UNKNOWN)
            goto unknown_format;
    }

    if (!gst_structure_get_int(structure, "width", &width)) {
        if (fmt != GST_VIDEO_FORMAT_UNKNOWN)
            goto no_width;
    }

    if (!gst_structure_get_int(structure, "height", &height)) {
        if (fmt != GST_VIDEO_FORMAT_UNKNOWN)
            goto no_height;
    }

    gst_video_info_init(info);

    info->finfo = gst_video_format_get_info(fmt);
    info->width = width;
    info->height = height;

    if (gst_structure_get_fraction(structure, "framerate", &fps_n, &fps_d)) {
        if (fps_n == 0) {
            info->flags |= GST_VIDEO_FLAG_VARIABLE_FPS;
            gst_structure_get_fraction(structure, "max-framerate", &fps_n, &fps_d);
        }

        info->fps_n = fps_n;
        info->fps_d = fps_d;
    } else {
        info->fps_n = 0;
        info->fps_d = 1;
    }

    if (gst_structure_get_fraction(structure, "pixel-aspect-ratio", &par_n, &par_d)) {
        info->par_d = par_d;
        info->par_n = par_n;
    } else {
        info->par_d = 1;
        info->par_n = 1;
    }

    if ((s = gst_structure_get_string(structure, "interlace-mode")))
        info->interlace_mode = gst_video_interlace_mode_from_string(s);
    else
        info->interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

    if (info->interlace_mode == GST_VIDEO_INTERLACE_MODE_ALTERNATE && 
        fmt != GST_VIDEO_FORMAT_UNKNOWN) {
            GstCapsFeatures *f;

            f = gst_caps_get_features(caps, 0);

            if (!f || gst_caps_features_contains(f, GST_CAPS_FEATURE_FORMAT_INTERLACED))
                goto alternate_no_feature;
    }

    if (GST_VIDEO_INFO_IS_INTERLACED(info) && (s = gst_structure_get_string(structure, "field-order"))) {
        GST_VIDEO_INFO_FIELD_ORDER(info) = gst_video_field_order_from_string(s);
    } else {
        GST_VIDEO_INFO_FIELD_ORDER(info) = GST_VIDEO_FIELD_ORDER_UNKNOWN;
    }

    if ((s = gst_structure_get_string(structure, "multiview-mode"))) {
        GST_VIDEO_INFO_MULTIVIEW_MODE(info) = gst_video_multiview_mode_from_caps_string(s);
    } else {
        GST_VIDEO_INFO_MULTIVIEW_MODE(info) = GST_VIDEO_MULTIVIEW_MODE_NONE;
    }

    if (gst_structure_get_flagset(structure, "multiview-flags", &multiview_flags, NULL)) {
        GST_VIDEO_INFO_MULTIVIEW_FLAGS(info) = multiview_flags;
    }

    if (!gst_structure_get_int(structure, "views", &info->views)) {
        info->views = 1;
    }

    if ((s = gst_structure_get_string(structure, "chroma-site"))) {
        info->chroma_site = gst_video_chroma_from_string(s);
    } else {
        info->chroma_site = GST_VIDEO_CHROMA_SITE_UNKNOWN;
    }

    if ((s = gst_structure_get_string(structure, "colorimetry"))) {
        if (!gst_video_colorimetry_from_string(&info->colorimetry, s)) {
            GST_WARNING("unparsable coloimetry, using default");
            set_default_colorimetry(info);
        } else if (!validate_colorimetry(info)) {
            GST_WARNING("invalid colorimetry, using default");
            set_default_colorimetry(info);
        } else {
            if (GST_VIDEO_FORMAT_INFO_IS_RGB(info->finfo) &&
                info->colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_RGB) {
                    GST_WARNING("invalid matrix %d for RGB format, using RGB", info->colorimetry.matrix);
                    info->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
                }
        }
    } else {
        GST_DEBUG("no colorimetry, using default");
        set_default_colorimetry(info);
    }

    if (!fill_planes(info))
        return -1;

    return 0;

no_format:
    GST_ERROR("no format given");
    return -1;

unknown_format:
    GST_ERROR("unknown format '%s' given", s);
    return -1;

no_width:
    GST_ERROR("no width");
    return -1;

no_height:
    GST_ERROR("no height");
    return -1;

alternate_no_feature:
    GST_ERROR("caps has 'interlace-mode=alternate' but doesn't have the Interlaced feature");
    return -1;
}

void gst_sunxiv4l2_set_camera_index(gpointer v4l2handle, gint idx)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    handle->camera.camera_index = idx;
}

gint gst_sunxiv4l2_get_camera_index(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    return handle->camera.camera_index;
}

gboolean gst_sunxiv4l2_is_open(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    handle->v4l2_fd ? TRUE : FALSE;
}

guint gst_sunxiv4l2_fps_n(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->fps_n;
}

guint gst_sunxiv4l2_fps_d(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->fps_d;
}


GstFlowReturn gst_sunxiv4l2_flush_all_buffer(gpointer v4l2handle)
{
    SUNXIV4l2Handle *handle = v4l2handle;
    gint i;
    struct v4l2_buffer buf;
    GstFlowReturn ret = GST_FLOW_OK;

    for (i = 0; i < handle->camera.buffer_count; i++) {
        memset(&buf, 0, sizeof(struct v4l2_buffer));

        buf.type = handle->camera.type;
        buf.memory = handle->camera.memory_mode;
        buf.index = i;

        if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            buf.length = handle->camera.nplanes;
            buf.m.planes = (struct v4l2_plane *)calloc(buf.length, sizeof(struct v4l2_plane));
        }

        ret = ioctl(handle->v4l2_fd, VIDIOC_QBUF, &buf);

        if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            free(buf.m.planes);
        
        if (ret < 0) {
            ret = GST_FLOW_ERROR;
            break;
        }
    }

    return ret;
}

gint
gst_sunxiv4l2_camera_qbuf(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gint idx)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->camera.ops.qbuf(handle, v4l2_buf, idx);
}

gint
gst_sunxiv4l2_camera_dqbuf(gpointer v4l2handle, struct v4l2_buffer *v4l2_buf, gint idx)
{
    SUNXIV4l2Handle *handle = v4l2handle;

    return handle->camera.ops.dqbuf(handle, v4l2_buf, idx);
}

gpointer
gst_sunxiv4l2_camera_pick_buffer(gpointer data, gint idx, gpointer v4l2handle)
{
    SunxiV4l2camera_mem_block *blk = data;
    SUNXIV4l2Handle *handle = v4l2handle;
    int ret;
    struct timeval tv;
    fd_set fds;

    g_return_val_if_fail(blk->initialized == TRUE, NULL);
    g_return_val_if_fail(handle != NULL, NULL);

    if (handle->streamon == TRUE) {
        if (blk->used == FALSE) {
            blk->v4l2_buf.type = handle->camera.type;
            blk->v4l2_buf.index = idx;
            if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
                blk->v4l2_buf.length = handle->camera.nplanes;
                blk->v4l2_buf.m.planes = (struct v4l2_plane *)calloc(blk->v4l2_buf.length, sizeof(struct v4l2_plane));
            }
            blk->used = TRUE;
        }

        FD_ZERO(&fds);
        FD_SET(handle->v4l2_fd, &fds);

        ret = select(handle->v4l2_fd + 1, &fds, NULL, NULL, NULL);

        if (ret < 0) {
            GST_ERROR("WAIT CAMERA DATA FAILED.");
            return NULL;
        }

        ret = gst_sunxiv4l2_camera_dqbuf(handle, &blk->v4l2_buf, 0);

        if (ret < 0) {
            return NULL;
        }
    }

    return blk->start[idx];
}

void gst_sunxiv4l2_camera_ref_buffer(gpointer data, gint idx, gpointer v4l2handle)
{
    SunxiV4l2camera_mem_block *blk = data;
    SUNXIV4l2Handle *handle = v4l2handle;

    if (handle->streamon == TRUE) {
        if (blk->used == FALSE) {
            blk->v4l2_buf.type = handle->camera.type;
            blk->v4l2_buf.index = idx;
            if (handle->type == V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
                blk->v4l2_buf.length = handle->camera.nplanes;
                blk->v4l2_buf.m.planes = (struct v4l2_plane *)calloc(blk->v4l2_buf.length, sizeof(struct v4l2_plane));
            }
            blk->used = TRUE;        
        }

        gst_sunxiv4l2_camera_qbuf(handle, &blk->v4l2_buf, idx);
    }
}
