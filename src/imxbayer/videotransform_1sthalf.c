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
#include "trans_common.h"

#define GST_CAT_DEFAULT imx_bayer_1sthalf_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);



#define GST_TYPE_IMX_BAYER1ST_HALF   (imx_bayer_1sthalf_get_type())
#define GST_IMXBAYER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_IMX_BAYER1ST_HALF,GstImxBayer1stHalf))
#define GST_IS_IMXBAYER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_IMX_BAYER1ST_HALF))
#define GST_IMXBAYER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass) ,GST_TYPE_IMX_BAYER1ST_HALF,GstImxBayer1stHalfClass))
#define GST_IS_IMXBAYER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass) ,GST_TYPE_IMX_BAYER1ST_HALF))
#define GST_IMXBAYER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj) ,GST_TYPE_IMX_BAYER1ST_HALF,GstImxBayer1stHalfClass))
typedef struct _GstImxBayer1stHalf GstImxBayer1stHalf;
typedef struct _GstImxBayer1stHalfClass GstImxBayer1stHalfClass;

typedef void (*GstImxBayer1stHalfProcessFunc) (GstImxBayer1stHalf *, guint8 *, guint);

struct _GstImxBayer1stHalf
{
  GstBaseTransform basetransform;

  /* < private > */
  GstVideoInfo info;
  int width;
  int height;
  int in_fmt;
  unsigned int fb;
  unsigned int fbset;
  unsigned int extbuf;
  float red;
  float green;
  float blue;
  unsigned int chrom;
  GstImxEglVivTransGLES2Renderer *renderer;
};

struct _GstImxBayer1stHalfClass
{
  GstBaseTransformClass parent;
};

/* dummy */
#define	SRC_CAPS \
  GST_VIDEO_CAPS_MAKE ("{ GRAY8 }")

#define SINK_CAPS "video/x-bayer,format=(string){bggr,grbg,gbrg,rggb}," \
  "width=(int)[1,1920],height=(int)[1,1080],framerate=(fraction)[0/1,MAX]"


GType imx_bayer_1sthalf_get_type (void);

#define imx_bayer_1sthalf_parent_class parent_class
G_DEFINE_TYPE (GstImxBayer1stHalf, imx_bayer_1sthalf, GST_TYPE_BASE_TRANSFORM);

static void imx_bayer_1sthalf_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void imx_bayer_1sthalf_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean imx_bayer_1sthalf_set_caps (GstBaseTransform * filter,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn imx_bayer_1sthalf_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static void imx_bayer_1sthalf_reset (GstImxBayer1stHalf * filter);
static GstCaps *imx_bayer_1sthalf_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean imx_bayer_1sthalf_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static gboolean imx_bayer_1sthalf_start(GstBaseTransform *base);
static gboolean imx_bayer_1sthalf_stop(GstBaseTransform *base);


static void
imx_bayer_1sthalf_class_init (GstImxBayer1stHalfClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = imx_bayer_1sthalf_set_property;
  gobject_class->get_property = imx_bayer_1sthalf_get_property;

  gst_element_class_set_static_metadata (gstelement_class,
      "Bayer to BGRA/I420 decoder for cameras", "Filter/Converter/Video",
      "Converts video/x-bayer to video/x-raw",
      "Naoki Aizu <n-aizu@pseudoterminal.org>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (SRC_CAPS)));
  gst_element_class_add_pad_template (gstelement_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string (SINK_CAPS)));

  GST_BASE_TRANSFORM_CLASS (klass)->transform_caps =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_transform_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->get_unit_size =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_get_unit_size);
  GST_BASE_TRANSFORM_CLASS (klass)->set_caps =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_set_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->transform =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_transform);
  GST_BASE_TRANSFORM_CLASS (klass)->start =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_start);
  GST_BASE_TRANSFORM_CLASS (klass)->stop =
      GST_DEBUG_FUNCPTR (imx_bayer_1sthalf_stop);

  GST_DEBUG_CATEGORY_INIT (imx_bayer_1sthalf_debug, "imxbayer", 0,
      "imxbayer element");

  imx_bayer_install_proverty1(gobject_class);
  imx_bayer_install_proverty2(gobject_class);
}

static void
imx_bayer_1sthalf_init (GstImxBayer1stHalf * filter)
{
  imx_bayer_1sthalf_reset (filter);
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (filter), TRUE);
}

static void
imx_bayer_1sthalf_set_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED const GValue * value, GParamSpec * pspec)
{
  GstImxBayer1stHalf *imxbayer = GST_IMXBAYER (object);

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
    case PROP_CHROM_VALUE:
      imxbayer->chrom = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
imx_bayer_1sthalf_get_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED GValue * value, GParamSpec * pspec)
{
  GstImxBayer1stHalf *imxbayer = GST_IMXBAYER (object);

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
    case PROP_CHROM_VALUE:
      g_value_set_uint(value, imxbayer->chrom);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
imx_bayer_1sthalf_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstImxBayer1stHalf *imxbayer = GST_IMXBAYER (base);
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

  if (gst_imx_egl_viv_trans_gles2_renderer_setup(imxbayer->renderer,
												imxbayer->width,
												imxbayer->height,
												imxbayer->in_fmt,
												imxbayer->red,
												imxbayer->green,
												imxbayer->blue,
												imxbayer->fbset,
												imxbayer->extbuf,
												imxbayer->chrom) == FALSE) {
	  return FALSE;
  }

  /* To cater for different RGB formats, we need to set params for later */
  gst_video_info_from_caps (&info, outcaps);
  imxbayer->info = info;

  return TRUE;
}

static void
imx_bayer_1sthalf_reset (GstImxBayer1stHalf * filter)
{
  filter->width = 0;
  filter->height = 0;
  filter->red = DEFAULT_RED_VAL;
  filter->green = DEFAULT_GREEN_VAL;
  filter->blue = DEFAULT_BLUE_VAL;
  filter->fb = DEFAULT_FBDEV_NUM;
  filter->fbset = DEFAULT_FBSET;
  filter->extbuf = DEFAULT_EXTBUF;
  filter->chrom = DEFAULT_CHROM_VAL;

  gst_video_info_init (&filter->info);
}

static GstCaps *
imx_bayer_1sthalf_transform_caps (G_GNUC_UNUSED GstBaseTransform * base,
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
imx_bayer_1sthalf_get_unit_size (GstBaseTransform * base, GstCaps * caps,
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
      /* dummy */
      *size = width * height;
      return TRUE;
    }

  }
  GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
      ("Incomplete caps, some required field missing"));
  return FALSE;
}


static GstFlowReturn
imx_bayer_1sthalf_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
	GstImxBayer1stHalf *imxbayer = GST_IMXBAYER(base);
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

static gboolean imx_bayer_1sthalf_start(GstBaseTransform *base)
{
	gboolean ret = TRUE;
	GstImxBayer1stHalf *imxbayer = GST_IMXBAYER(base);
	char fb[4];

	snprintf(fb, sizeof(fb), "%u", imxbayer->fb);
	imxbayer->renderer = gst_imx_egl_viv_trans_gles2_renderer_create(fb);
	if (imxbayer->renderer == NULL) {
		ret = FALSE;
	}

	return ret;
}

static gboolean imx_bayer_1sthalf_stop(GstBaseTransform *base)
{
	GstImxBayer1stHalf *imxbayer = GST_IMXBAYER(base);

	gst_imx_egl_viv_trans_gles2_renderer_destroy(imxbayer->renderer);
	imxbayer->renderer = NULL;
	return TRUE;
}
