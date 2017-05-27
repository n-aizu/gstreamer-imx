#ifndef GST_IMX_EGL_VIV_TRANS_GLES2_RENDERER_H
#define GST_IMX_EGL_VIV_TRANS_GLES2_RENDERER_H

#include <gst/gst.h>
#include <gst/video/video.h>


G_BEGIN_DECLS


typedef struct _GstImxEglVivTransGLES2Renderer GstImxEglVivTransGLES2Renderer;
typedef struct _GstImxEglVivTransFb GstImxEglVivTransFb;

enum
{
  GST_EGL_TRANS_FORMAT_BGGR = 0,
  GST_EGL_TRANS_FORMAT_GBRG,
  GST_EGL_TRANS_FORMAT_GRBG,
  GST_EGL_TRANS_FORMAT_RGGB,
  GST_EGL_TRANS_FORMAT_BGRA,
  GST_EGL_TRANS_FORMAT_I420,
  GST_EGL_TRANS_FORMAT_UYVY
};

enum
{
  GST_DEMOSAIC_MHC = 0,
  GST_DEMOSAIC_EDGE,
  GST_DEMOSAIC_EDGE_YUV,
  GST_DEMOSAIC_EDGE_YUV_LESSER
};

GstImxEglVivTransGLES2Renderer* gst_imx_egl_viv_trans_gles2_renderer_create(char const *native_display_name);
void gst_imx_egl_viv_trans_gles2_renderer_destroy(GstImxEglVivTransGLES2Renderer *renderer);
gboolean gst_imx_egl_viv_trans_gles2_renderer_setup(GstImxEglVivTransGLES2Renderer *renderer, GstPad *push_pad, GstCaps *out_caps, guint width, guint height, guint in_fmt, guint out_fmt, gfloat red_coeff, gfloat green_coeff, gfloat blue_coeff, gboolean fbset, gboolean phys_alloc, gboolean hd_lite, guint demosaic);
gboolean gst_imx_egl_viv_trans_gles2_renderer_render_frame(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *src, GstBuffer *dest);


G_END_DECLS


#endif

