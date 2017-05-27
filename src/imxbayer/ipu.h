#ifndef GST_IMX_BAYER_IPU_H
#define GST_IMX_BAYER_IPU_H

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

gboolean gst_imx_bayer_ipu_open(void);
void gst_imx_bayer_ipu_close(void);
int gst_imx_bayer_ipu_get_fd(void);
gboolean gst_imx_bayer_ipu_yuv_init(size_t size, unsigned long *ipu_paddr, void **ipu_vaddr);
gboolean gst_imx_bayer_ipu_yuv_end(size_t size, unsigned long ipu_paddr, void *ipu_vaddr);
gboolean gst_imx_bayer_ipu_yuv_conv(int width, int height, unsigned long fb_paddr, unsigned long dst_paddr, void *ipu_vaddr, void *dest, unsigned int in_fmt, unsigned int out_fmt, size_t out_size);
gboolean gst_imx_bayer_ipu_yuv_conv_div(int width, int height, unsigned long fb_paddr, unsigned long dst_paddr, void *ipu_vaddr, void *dest, unsigned int in_fmt, unsigned int out_fmt, size_t out_size, int hdiv, int vdiv);

G_END_DECLS

#endif
