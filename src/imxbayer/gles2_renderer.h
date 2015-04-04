#ifndef GST_IMX_EGL_VIV_TRANS_GLES2_RENDERER_H
#define GST_IMX_EGL_VIV_TRANS_GLES2_RENDERER_H

#include <gst/gst.h>
#include <gst/video/video.h>


G_BEGIN_DECLS


typedef struct _GstImxEglVivTransGLES2Renderer GstImxEglVivTransGLES2Renderer;

enum
{
  GST_EGL_TRANS_FORMAT_BGGR = 0,
  GST_EGL_TRANS_FORMAT_GBRG,
  GST_EGL_TRANS_FORMAT_GRBG,
  GST_EGL_TRANS_FORMAT_RGGB,
  GST_EGL_TRANS_FORMAT_BGRA,
  GST_EGL_TRANS_FORMAT_I420
};

GstImxEglVivTransGLES2Renderer* gst_imx_egl_viv_trans_gles2_renderer_create(char const *native_display_name);
void gst_imx_egl_viv_trans_gles2_renderer_destroy(GstImxEglVivTransGLES2Renderer *renderer);
gboolean gst_imx_egl_viv_trans_gles2_renderer_setup(GstImxEglVivTransGLES2Renderer *renderer, int width, int height, int in_fmt, int out_fmt, float red_coeff, float green_coeff, float blue_coeff, unsigned int fbset, unsigned int extbuf, unsigned int chrom);
gboolean gst_imx_egl_viv_trans_gles2_renderer_render_frame(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *src, GstBuffer *dest);


G_END_DECLS


#endif

