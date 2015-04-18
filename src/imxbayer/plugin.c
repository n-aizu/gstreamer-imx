/* eglvivsink GStreamer 1.0 plugin definition
 * Copyright (C) 2013  Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <config.h>
#include <gst/gst.h>

extern GType imx_bayer_get_type (void);
extern GType imx_bayer_1sthalf_get_type (void);
extern GType imx_bayer_2ndhalf_get_type (void);

static gboolean plugin_init(GstPlugin *plugin)
{
	gboolean ret = TRUE;

	ret = ret && gst_element_register(plugin, "imxbayer", GST_RANK_PRIMARY + 1, imx_bayer_get_type());
	ret = ret && gst_element_register(plugin, "imxbayer1sthalf", GST_RANK_PRIMARY + 1, imx_bayer_1sthalf_get_type());
	ret = ret && gst_element_register(plugin, "imxbayer2ndhalf", GST_RANK_PRIMARY + 1, imx_bayer_2ndhalf_get_type());

	return ret;
}



GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	imxbayer,
	"Elements to convert Bayer images(with i.MX HW)",
	plugin_init,
	VERSION,
	"LGPL",
	GST_PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)

