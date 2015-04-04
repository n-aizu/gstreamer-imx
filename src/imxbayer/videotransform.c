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
#include <gst/video/video.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gles2_renderer.h"
#include "../common/phys_mem_meta.h"
#include "ipu_allocator.h"

#define GST_CAT_DEFAULT imx_bayer_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);



#define GST_TYPE_IMX_BAYER            (imx_bayer_get_type())
#define GST_IMXBAYER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_IMX_BAYER,GstImxBayer))
#define GST_IS_IMXBAYER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_IMX_BAYER))
#define GST_IMXBAYER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass) ,GST_TYPE_IMX_BAYER,GstImxBayerClass))
#define GST_IS_IMXBAYER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass) ,GST_TYPE_IMX_BAYER))
#define GST_IMXBAYER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj) ,GST_TYPE_IMX_BAYER,GstImxBayerClass))
typedef struct _GstImxBayer GstImxBayer;
typedef struct _GstImxBayerClass GstImxBayerClass;

typedef void (*GstImxBayerProcessFunc) (GstImxBayer *, guint8 *, guint);

struct _GstImxBayer
{
  GstBaseTransform basetransform;

  /* < private > */
  GstVideoInfo info;
  int width;
  int height;
  int in_fmt;
  int out_fmt;
  unsigned int fb;
  unsigned int fbset;
  unsigned int extbuf;
  float red;
  float green;
  float blue;
  GstImxEglVivTransGLES2Renderer *renderer;
};

struct _GstImxBayerClass
{
  GstBaseTransformClass parent;
};

#define	SRC_CAPS \
  GST_VIDEO_CAPS_MAKE ("{ BGRA, I420 }")

#define SINK_CAPS "video/x-bayer,format=(string){bggr,grbg,gbrg,rggb}," \
  "width=(int)[1,1920],height=(int)[1,1080],framerate=(fraction)[0/1,MAX]"

#define DEFAULT_FBDEV_NUM 0
#define DEFAULT_FBSET 0
#define DEFAULT_EXTBUF 0

enum
{
  PROP_0,
  PROP_FBDEV_NUM,
  PROP_FBSET,
  PROP_EXTBUF,
  PROP_RED_FILTER,
  PROP_GREEN_FILTER,
  PROP_BLUE_FILTER,
};

GType imx_bayer_get_type (void);

#define imx_bayer_parent_class parent_class
G_DEFINE_TYPE (GstImxBayer, imx_bayer, GST_TYPE_BASE_TRANSFORM);

static void imx_bayer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void imx_bayer_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean imx_bayer_set_caps (GstBaseTransform * filter,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean imx_bayer_decide_allocation(GstBaseTransform *transform, GstQuery *query);
static GstFlowReturn imx_bayer_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static void imx_bayer_reset (GstImxBayer * filter);
static GstCaps *imx_bayer_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean imx_bayer_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static gboolean imx_bayer_start(GstBaseTransform *base);
static gboolean imx_bayer_stop(GstBaseTransform *base);


static void
imx_bayer_class_init (GstImxBayerClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = imx_bayer_set_property;
  gobject_class->get_property = imx_bayer_get_property;

  gst_element_class_set_static_metadata (gstelement_class,
      "Bayer to RGB decoder for cameras", "Filter/Converter/Video",
      "Converts video/x-bayer to video/x-raw",
      "William Brack <wbrack@mmm.com.hk>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (SRC_CAPS)));
  gst_element_class_add_pad_template (gstelement_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string (SINK_CAPS)));

  GST_BASE_TRANSFORM_CLASS (klass)->transform_caps =
      GST_DEBUG_FUNCPTR (imx_bayer_transform_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->get_unit_size =
      GST_DEBUG_FUNCPTR (imx_bayer_get_unit_size);
  GST_BASE_TRANSFORM_CLASS (klass)->set_caps =
      GST_DEBUG_FUNCPTR (imx_bayer_set_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->decide_allocation =
      GST_DEBUG_FUNCPTR (imx_bayer_decide_allocation);
  GST_BASE_TRANSFORM_CLASS (klass)->transform =
      GST_DEBUG_FUNCPTR (imx_bayer_transform);
  GST_BASE_TRANSFORM_CLASS (klass)->start =
      GST_DEBUG_FUNCPTR (imx_bayer_start);
  GST_BASE_TRANSFORM_CLASS (klass)->stop =
      GST_DEBUG_FUNCPTR (imx_bayer_stop);

  GST_DEBUG_CATEGORY_INIT (imx_bayer_debug, "imxbayer", 0,
      "imxbayer element");

	g_object_class_install_property(
		gobject_class,
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

	g_object_class_install_property(
		gobject_class,
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
		gobject_class,
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
		gobject_class,
		PROP_RED_FILTER,
		g_param_spec_float(
			"red",
			"Red color filter",
			"Red color filter value",
			0.1, 5.0,
			1.0,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		gobject_class,
		PROP_GREEN_FILTER,
		g_param_spec_float(
			"green",
			"Green color filter",
			"Green color filter value",
			0.1, 5.0,
			1.0,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		gobject_class,
		PROP_BLUE_FILTER,
		g_param_spec_float(
			"blue",
			"Blue color filter",
			"Blue color filter value",
			0.1, 5.0,
			1.0,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
}

static void
imx_bayer_init (GstImxBayer * filter)
{
  imx_bayer_reset (filter);
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (filter), TRUE);
}

static void
imx_bayer_set_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED const GValue * value, GParamSpec * pspec)
{
  GstImxBayer *imxbayer = GST_IMXBAYER (object);

  switch (prop_id) {
    case PROP_FBDEV_NUM:
      imxbayer->fb = g_value_get_uint(value);
      break;
    case PROP_FBSET:
      imxbayer->fbset = g_value_get_uint(value);
      break;
    case PROP_EXTBUF:
      imxbayer->extbuf = g_value_get_uint(value);
      break;
    case PROP_RED_FILTER:
      imxbayer->red = g_value_get_float(value);
      break;
    case PROP_GREEN_FILTER:
      imxbayer->green = g_value_get_float(value);
      break;
    case PROP_BLUE_FILTER:
      imxbayer->blue = g_value_get_float(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
imx_bayer_get_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED GValue * value, GParamSpec * pspec)
{
  GstImxBayer *imxbayer = GST_IMXBAYER (object);

  switch (prop_id) {
    case PROP_FBDEV_NUM:
      g_value_set_uint(value, imxbayer->fb);
      break;
    case PROP_FBSET:
      g_value_set_uint(value, imxbayer->fbset);
      break;
    case PROP_EXTBUF:
      g_value_set_uint(value, imxbayer->extbuf);
      break;
    case PROP_RED_FILTER:
      g_value_set_float(value, imxbayer->red);
      break;
    case PROP_GREEN_FILTER:
      g_value_set_float(value, imxbayer->green);
      break;
    case PROP_BLUE_FILTER:
      g_value_set_float(value, imxbayer->blue);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
imx_bayer_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstImxBayer *imxbayer = GST_IMXBAYER (base);
  GstStructure *structure;
  const char *format;
  GstVideoInfo info;

  GST_DEBUG ("in caps %" GST_PTR_FORMAT " out caps %" GST_PTR_FORMAT, incaps,
      outcaps);

  structure = gst_caps_get_structure (incaps, 0);

  gst_structure_get_int (structure, "width", &imxbayer->width);
  gst_structure_get_int (structure, "height", &imxbayer->height);

  format = gst_structure_get_string (structure, "format");
  if (g_str_equal (format, "bggr")) {
    imxbayer->in_fmt = GST_EGL_TRANS_FORMAT_BGGR;
  } else if (g_str_equal (format, "gbrg")) {
    imxbayer->in_fmt = GST_EGL_TRANS_FORMAT_GBRG;
  } else if (g_str_equal (format, "grbg")) {
    imxbayer->in_fmt = GST_EGL_TRANS_FORMAT_GRBG;
  } else if (g_str_equal (format, "rggb")) {
    imxbayer->in_fmt = GST_EGL_TRANS_FORMAT_RGGB;
  } else {
    return FALSE;
  }

  structure = gst_caps_get_structure (outcaps, 0);

  format = gst_structure_get_string (structure, "format");
  if (g_str_equal (format, "BGRA")) {
    imxbayer->out_fmt = GST_EGL_TRANS_FORMAT_BGRA;
  } else if (g_str_equal (format, "I420")) {
    imxbayer->out_fmt = GST_EGL_TRANS_FORMAT_I420;
  } else {
    return FALSE;
  }

  if (gst_imx_egl_viv_trans_gles2_renderer_setup(imxbayer->renderer,
												imxbayer->width,
												imxbayer->height,
												imxbayer->in_fmt,
												imxbayer->out_fmt,
												imxbayer->red,
												imxbayer->green,
												imxbayer->blue,
												imxbayer->fbset,
												imxbayer->extbuf) == FALSE) {
	  return FALSE;
  }

  /* To cater for different RGB formats, we need to set params for later */
  gst_video_info_from_caps (&info, outcaps);
  imxbayer->info = info;

  return TRUE;
}

static void
imx_bayer_reset (GstImxBayer * filter)
{
  filter->width = 0;
  filter->height = 0;
  filter->red = 1.0;
  filter->green = 1.0;
  filter->blue = 1.0;
  filter->fbset = 0;
  filter->extbuf = 0;

  gst_video_info_init (&filter->info);
}

static GstCaps *
imx_bayer_transform_caps (G_GNUC_UNUSED GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, G_GNUC_UNUSED GstCaps * filter)
{
  GstStructure *structure;
  GstCaps *newcaps;
  GstStructure *newstruct;

  GST_DEBUG_OBJECT (caps, "transforming caps (from)");

  structure = gst_caps_get_structure (caps, 0);

  if (direction == GST_PAD_SRC) {
    newcaps = gst_caps_from_string ("video/x-bayer,"
        "format=(string){bggr,grbg,gbrg,rggb}");
  } else {
    newcaps = gst_caps_new_empty_simple ("video/x-raw");
  }
  newstruct = gst_caps_get_structure (newcaps, 0);

  gst_structure_set_value (newstruct, "width",
      gst_structure_get_value (structure, "width"));
  gst_structure_set_value (newstruct, "height",
      gst_structure_get_value (structure, "height"));
  gst_structure_set_value (newstruct, "framerate",
      gst_structure_get_value (structure, "framerate"));

  GST_DEBUG_OBJECT (newcaps, "transforming caps (into)");

  return newcaps;
}

static gboolean
imx_bayer_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    gsize * size)
{
  GstStructure *structure;
  int width;
  int height;
  const char *name;

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_get_int (structure, "width", &width) &&
      gst_structure_get_int (structure, "height", &height)) {
    name = gst_structure_get_name (structure);
    /* Our name must be either video/x-bayer video/x-raw */
    if (strcmp (name, "video/x-raw")) {
      *size = GST_ROUND_UP_4 (width) * height;
      return TRUE;
    } else {
      /* For output, calculate according to format (always 32 bits) */
      *size = width * height * 4;
      return TRUE;
    }

  }
  GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
      ("Incomplete caps, some required field missing"));
  return FALSE;
}


static gboolean imx_bayer_decide_allocation(G_GNUC_UNUSED GstBaseTransform *transform, GstQuery *query)
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


static GstFlowReturn
imx_bayer_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
	GstImxBayer *imxbayer = GST_IMXBAYER(base);
	gboolean eglstat;
	GstFlowReturn flowstat = GST_FLOW_OK;
	
	eglstat = gst_imx_egl_viv_trans_gles2_renderer_render_frame(
					imxbayer->renderer,
					inbuf,
					outbuf);
	if (eglstat == FALSE) {
		flowstat = GST_FLOW_ERROR;
	}

	return flowstat;
}

static gboolean imx_bayer_start(GstBaseTransform *base)
{
	gboolean ret = TRUE;
	GstImxBayer *imxbayer = GST_IMXBAYER(base);
	char fb[4];

	snprintf(fb, sizeof(fb), "%u", imxbayer->fb);
	imxbayer->renderer = gst_imx_egl_viv_trans_gles2_renderer_create(fb);
	if (imxbayer->renderer == NULL) {
		ret = FALSE;
	}

	return ret;
}

static gboolean imx_bayer_stop(GstBaseTransform *base)
{
	GstImxBayer *imxbayer = GST_IMXBAYER(base);

	gst_imx_egl_viv_trans_gles2_renderer_destroy(imxbayer->renderer);
	imxbayer->renderer = NULL;
	return TRUE;
}
