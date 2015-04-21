#ifndef GST_IMX_TRANS_COMM_H
#define GST_IMX_TRANS_COMM_H

#include <gst/gst.h>
#include <gst/video/video.h>
#include "ipu_allocator.h"
#include "../common/phys_mem_meta.h"

G_BEGIN_DECLS

#define DEFAULT_FBDEV_NUM 0
#define DEFAULT_FBSET 0
#define DEFAULT_EXTBUF 0
#define DEFAULT_RED_VAL 1.0
#define DEFAULT_GREEN_VAL 1.0
#define DEFAULT_BLUE_VAL 1.0
#define DEFAULT_CHROM_VAL 128

enum
{
  PROP_0,
  PROP_FBDEV_NUM,
  PROP_FBSET,
  PROP_EXTBUF,
  PROP_RED_FILTER,
  PROP_GREEN_FILTER,
  PROP_BLUE_FILTER,
  PROP_CHROM_VALUE,
};


void imx_bayer_install_proverty1(GObjectClass *oclass);
void imx_bayer_install_proverty2(GObjectClass *oclass);
gboolean imx_bayer_decide_allocation_base(GstQuery *query);


G_END_DECLS

#endif
