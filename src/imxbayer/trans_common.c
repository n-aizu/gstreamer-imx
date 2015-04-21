/* 
 * GStreamer
 * Copyright (C) 2007 David Schleef <ds@schleef.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include "trans_common.h"


void imx_bayer_install_proverty1(GObjectClass *oclass)
{
	g_object_class_install_property(
		oclass,
		PROP_FBSET,
		g_param_spec_uint(
			"fbset",
			"FBIOPUT_VSCREENINFO ioctl",
			"Call FBIOPUT_VSCREENINFO ioctl(1 is call)",
			0, 1,
			DEFAULT_FBSET,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		oclass,
		PROP_EXTBUF,
		g_param_spec_uint(
			"extbuf",
			"Extended video frame buffer",
			"Camera driver support extended video frame buffer for debayer(1 is supported)",
			0, 1,
			DEFAULT_EXTBUF,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		oclass,
		PROP_RED_FILTER,
		g_param_spec_float(
			"red",
			"Red color filter",
			"Red color filter value",
			0.1, 5.0,
			DEFAULT_RED_VAL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		oclass,
		PROP_GREEN_FILTER,
		g_param_spec_float(
			"green",
			"Green color filter",
			"Green color filter value",
			0.1, 5.0,
			DEFAULT_GREEN_VAL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		oclass,
		PROP_BLUE_FILTER,
		g_param_spec_float(
			"blue",
			"Blue color filter",
			"Blue color filter value",
			0.1, 5.0,
			DEFAULT_BLUE_VAL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		oclass,
		PROP_CHROM_VALUE,
		g_param_spec_uint(
			"chrom",
			"chrominance",
			"chrominance value",
			0, 255,
			DEFAULT_CHROM_VAL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
}

void imx_bayer_install_proverty2(GObjectClass *oclass)
{
	g_object_class_install_property(
		oclass,
		PROP_FBDEV_NUM,
		g_param_spec_uint(
			"fbnum",
			"Framebuffer device number",
			"The device number of the framebuffer to render to",
			0, 31,
			DEFAULT_FBDEV_NUM,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
}


gboolean imx_bayer_decide_allocation_base(GstQuery *query)
{
	GstCaps *outcaps;
	GstBufferPool *pool = NULL;
	guint size, min = 0, max = 0;
	GstStructure *config;
	GstVideoInfo vinfo;
	gboolean update_pool;

	gst_query_parse_allocation(query, &outcaps, NULL);
	gst_video_info_init(&vinfo);
	gst_video_info_from_caps(&vinfo, outcaps);

	GST_DEBUG("num allocation pools: %d", gst_query_get_n_allocation_pools(query));

	/* Look for an allocator which can allocate physical memory buffers */
	if (gst_query_get_n_allocation_pools(query) > 0)
	{
		for (guint i = 0; i < gst_query_get_n_allocation_pools(query); ++i)
		{
			gst_query_parse_nth_allocation_pool(query, i, &pool, &size, &min, &max);
			if (gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM))
				break;
		}

		size = MAX(size, vinfo.size);
		update_pool = TRUE;
	}
	else
	{
		pool = NULL;
		size = vinfo.size;
		min = max = 0;
		update_pool = FALSE;
	}

	/* Either no pool or no pool with the ability to allocate physical memory buffers
	 * has been found -> create a new pool */
	if ((pool == NULL) || !gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM))
	{
		if (pool == NULL)
			GST_DEBUG("no pool present; creating new pool");
		else
			GST_DEBUG("no pool supports physical memory buffers; creating new pool");
		pool = gst_imx_bayer_ipu_create_bufferpool(outcaps, size, min, max);
	}
	else
	{
		config = gst_buffer_pool_get_config(pool);
		gst_buffer_pool_config_set_params(config, outcaps, size, min, max);
		gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM);
		gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
		gst_buffer_pool_set_config(pool, config);
	}

	GST_DEBUG(
		"pool config:  outcaps: %" GST_PTR_FORMAT "  size: %u  min buffers: %u  max buffers: %u",
		(gpointer)outcaps,
		size,
		min,
		max
	);

	if (update_pool)
		gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
	else
		gst_query_add_allocation_pool(query, pool, size, min, max);

	if (pool != NULL)
		gst_object_unref(pool);

	return TRUE;
}

