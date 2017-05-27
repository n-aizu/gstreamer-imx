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

#define GST_CAT_DEFAULT imx_bayer_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);



#define GST_TYPE_IMX_BAYER           (imx_bayer_get_type())
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
  guint width;
  guint height;
  guint in_fmt;
  guint out_fmt;
  guint demosaic;
  guint fb;
  gboolean fbset;
  gboolean phys_alloc;
  gboolean hd_lite;
  gfloat red;
  gfloat green;
  gfloat blue;
  GstImxEglVivTransGLES2Renderer *renderer;
};

struct _GstImxBayerClass
{
  GstBaseTransformClass parent;
};

#define	SRC_CAPS \
  GST_VIDEO_CAPS_MAKE ("{ BGRA, I420, UYVY }")

#define SINK_CAPS "video/x-bayer,format=(string){bggr,grbg,gbrg,rggb}," \
  "width=(int)[1,1920],height=(int)[1,1080],framerate=(fraction)[0/1,MAX]"

#define DEFAULT_FBDEV_NUM 0
#define DEFAULT_FBSET TRUE
#define DEFAULT_RED_VAL 1.0
#define DEFAULT_GREEN_VAL 1.0
#define DEFAULT_BLUE_VAL 1.0
#define DEFAULT_DEMOSAIC GST_DEMOSAIC_MHC
#define DEFAULT_PHYS_ALLOC TRUE
#define DEFAULT_HD_LITE FALSE

enum
{
  PROP_0,
  PROP_FBDEV_NUM,
  PROP_FBSET,
  PROP_RED_FILTER,
  PROP_GREEN_FILTER,
  PROP_BLUE_FILTER,
  PROP_DEMOSAIC,
  PROP_PHYS_ALLOC,
  PROP_HD_LITE,
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
      "Bayer to BGRA/I420/UYVY decoder for cameras", "Filter/Converter/Video",
      "Converts video/x-bayer to video/x-raw",
      "Naoki Aizu <n-aizu@pseudoterminal.org>");

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
		g_param_spec_boolean(
			"fbset",
			"FBIOPUT_VSCREENINFO ioctl",
			"Call FBIOPUT_VSCREENINFO ioctl",
			DEFAULT_FBSET,
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
			DEFAULT_RED_VAL,
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
			DEFAULT_GREEN_VAL,
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
			DEFAULT_BLUE_VAL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		gobject_class,
		PROP_DEMOSAIC,
		g_param_spec_uint(
			"demosaic",
			"demosaic(debayer) algorithm",
			"demosaic algorithm(0:High-Quality Linear Interpolation(Malvar-He-Cutler)/"
				"1:Edge Aware Weighted bilinear interpolation/"
				"2:Edge Aware Weighted(in YUV color space)/"
				"3:Edge Aware Weighted(in YUV color space/less expensive version))",
			0, 3,
			DEFAULT_DEMOSAIC,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		gobject_class,
		PROP_PHYS_ALLOC,
		g_param_spec_boolean(
			"physalloc",
			"use imx phys mem allocator",
			"use imx phys mem allocator",
			DEFAULT_PHYS_ALLOC,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	/* https://en.wikipedia.org/wiki/HD_Lite */
	g_object_class_install_property(
		gobject_class,
		PROP_HD_LITE,
		g_param_spec_boolean(
			"hdlite",
			"HD Lite",
			"HD Lite(1920x1080 to 1280x1080)",
			DEFAULT_HD_LITE,
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
      imxbayer->fbset = g_value_get_boolean(value);
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
    case PROP_DEMOSAIC:
      imxbayer->demosaic = g_value_get_uint(value);
      break;
    case PROP_PHYS_ALLOC:
      imxbayer->phys_alloc = g_value_get_boolean(value);
      break;
    case PROP_HD_LITE:
      imxbayer->hd_lite = g_value_get_boolean(value);
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
      g_value_set_boolean(value, imxbayer->fbset);
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
    case PROP_DEMOSAIC:
      g_value_set_uint(value, imxbayer->demosaic);
      break;
    case PROP_PHYS_ALLOC:
      g_value_set_boolean(value, imxbayer->phys_alloc);
      break;
    case PROP_HD_LITE:
      g_value_set_boolean(value, imxbayer->hd_lite);
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

  gst_structure_get_int (structure, "width", (gint *)&imxbayer->width);
  gst_structure_get_int (structure, "height", (gint *)&imxbayer->height);

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
  } else if (g_str_equal (format, "UYVY")) {
    imxbayer->out_fmt = GST_EGL_TRANS_FORMAT_UYVY;
  } else {
    return FALSE;
  }

  if (gst_imx_egl_viv_trans_gles2_renderer_setup(imxbayer->renderer,
												base->srcpad,
												outcaps,
												imxbayer->width,
												imxbayer->height,
												imxbayer->in_fmt,
												imxbayer->out_fmt,
												imxbayer->red,
												imxbayer->green,
												imxbayer->blue,
												imxbayer->fbset,
												imxbayer->phys_alloc,
												imxbayer->hd_lite,
												imxbayer->demosaic) == FALSE) {
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
  filter->red = DEFAULT_RED_VAL;
  filter->green = DEFAULT_GREEN_VAL;
  filter->blue = DEFAULT_BLUE_VAL;
  filter->fbset = DEFAULT_FBSET;
  filter->phys_alloc = DEFAULT_PHYS_ALLOC;
  filter->hd_lite = DEFAULT_HD_LITE;
  filter->demosaic = DEFAULT_DEMOSAIC;

  gst_video_info_init (&filter->info);
}

static GstCaps *
imx_bayer_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, G_GNUC_UNUSED GstCaps * filter)
{
  GstImxBayer *imxbayer = GST_IMXBAYER (base);
  GstStructure *structure;
  GstCaps *newcaps;
  GstStructure *newstruct;
  gint width = 0, height = 0;

  GST_DEBUG_OBJECT (caps, "transforming caps (from)");

  structure = gst_caps_get_structure (caps, 0);
  gst_structure_get_int (structure, "width", &width);
  gst_structure_get_int (structure, "height", &height);

  if (direction == GST_PAD_SRC) {
    newcaps = gst_caps_from_string ("video/x-bayer,"
        "format=(string){bggr,grbg,gbrg,rggb}");
  } else {
    newcaps = gst_caps_new_empty_simple ("video/x-raw");
  }
  newstruct = gst_caps_get_structure (newcaps, 0);

  if ((imxbayer->demosaic == GST_DEMOSAIC_EDGE_YUV || imxbayer->demosaic == GST_DEMOSAIC_EDGE_YUV_LESSER) &&
    imxbayer->hd_lite != FALSE && direction == GST_PAD_SINK && width == 1920 && height == 1080) {
    GValue value = { 0, };

	g_value_init (&value, G_TYPE_INT);
	g_value_set_int (&value, 1280);
    gst_structure_set_value (newstruct, "width", &value);
  }
  else {
    gst_structure_set_value (newstruct, "width",
        gst_structure_get_value (structure, "width"));
  }
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

static GstFlowReturn
imx_bayer_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
	GstImxBayer *imxbayer = GST_IMXBAYER(base);
	gboolean eglstat;
	/* prtend to drop frame, and call gst_pad_push later */
	GstFlowReturn flowstat = GST_BASE_TRANSFORM_FLOW_DROPPED;
	
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
