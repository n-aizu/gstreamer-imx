#ifndef GST_IMX_EGL_VIV_TRANS_EGL_PLATFORM_H
#define GST_IMX_EGL_VIV_TRANS_EGL_PLATFORM_H

#include <gst/gst.h>
#include <gst/video/video.h>


G_BEGIN_DECLS

typedef struct _GstImxEglVivTransEGLPlatform GstImxEglVivTransEGLPlatform;


GstImxEglVivTransEGLPlatform* gst_imx_egl_viv_trans_egl_platform_create(gchar const *native_display_name);
void gst_imx_egl_viv_trans_egl_platform_destroy(GstImxEglVivTransEGLPlatform *platform);

gboolean gst_imx_egl_viv_trans_egl_platform_init_window(GstImxEglVivTransEGLPlatform *platform, gint x_coord, gint y_coord, guint width, guint height);
gboolean gst_imx_egl_viv_trans_egl_platform_shutdown_window(GstImxEglVivTransEGLPlatform *platform);
void gst_imx_egl_viv_trans_egl_platform_swap_buffers(GstImxEglVivTransEGLPlatform *platform);


G_END_DECLS


#endif

