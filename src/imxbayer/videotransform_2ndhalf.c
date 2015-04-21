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


#define GST_CAT_DEFAULT imx_bayer_2ndhalf_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);



#define GST_TYPE_IMX_BAYER_2ND_HALF          (imx_bayer_2ndhalf_get_type())
#define GST_IMXBAYER2ND_HALF(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_IMX_BAYER_2ND_HALF,GstImxBayer2ndHalf))
#define GST_IS_IMXBAYER2ND_HALF(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_IMX_BAYER_2ND_HALF))
#define GST_IMXBAYER2ND_HALF_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass) ,GST_TYPE_IMX_BAYER_2ND_HALF,GstImxBayer2ndHalfClass))
#define GST_IS_IMXBAYER2ND_HALF_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass) ,GST_TYPE_IMX_BAYER_2ND_HALF))
#define GST_IMXBAYER2ND_HALF_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj) ,GST_TYPE_IMX_BAYER_2ND_HALF,GstImxBayer2ndHalfClass))
typedef struct _GstImxBayer2ndHalf GstImxBayer2ndHalf;
typedef struct _GstImxBayer2ndHalfClass GstImxBayer2ndHalfClass;

typedef void (*GstImxBayer2ndHalfProcessFunc) (GstImxBayer2ndHalf *, guint8 *, guint);

struct _GstImxBayer2ndHalf
{
  GstBaseTransform basetransform;

  /* < private > */
  GstVideoInfo info;
  int width;
  int height;
  int out_fmt;
  unsigned int fb;
  GstImxEglVivTransFb *fbdata;
};

struct _GstImxBayer2ndHalfClass
{
  GstBaseTransformClass parent;
};

#define	SRC_CAPS \
  GST_VIDEO_CAPS_MAKE ("{ BGRA, I420 }")

/* dummy */
#define SINK_CAPS \
  GST_VIDEO_CAPS_MAKE ("{ GRAY8 }")


GType imx_bayer_2ndhalf_get_type (void);

#define imx_bayer_2ndhalf_parent_class parent_class
G_DEFINE_TYPE (GstImxBayer2ndHalf, imx_bayer_2ndhalf, GST_TYPE_BASE_TRANSFORM);

static void imx_bayer_2ndhalf_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void imx_bayer_2ndhalf_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean imx_bayer_2ndhalf_set_caps (GstBaseTransform * filter,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean imx_bayer_2ndhalf_decide_allocation(GstBaseTransform *transform, GstQuery *query);
static GstFlowReturn imx_bayer_2ndhalf_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static void imx_bayer_2ndhalf_reset (GstImxBayer2ndHalf * filter);
static GstCaps *imx_bayer_2ndhalf_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean imx_bayer_2ndhalf_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static gboolean imx_bayer_2ndhalf_start(GstBaseTransform *base);
static gboolean imx_bayer_2ndhalf_stop(GstBaseTransform *base);


static void
imx_bayer_2ndhalf_class_init (GstImxBayer2ndHalfClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = imx_bayer_2ndhalf_set_property;
  gobject_class->get_property = imx_bayer_2ndhalf_get_property;

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
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_transform_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->get_unit_size =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_get_unit_size);
  GST_BASE_TRANSFORM_CLASS (klass)->set_caps =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_set_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->decide_allocation =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_decide_allocation);
  GST_BASE_TRANSFORM_CLASS (klass)->transform =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_transform);
  GST_BASE_TRANSFORM_CLASS (klass)->start =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_start);
  GST_BASE_TRANSFORM_CLASS (klass)->stop =
      GST_DEBUG_FUNCPTR (imx_bayer_2ndhalf_stop);

  GST_DEBUG_CATEGORY_INIT (imx_bayer_2ndhalf_debug, "imxbayer", 0,
      "imxbayer element");

  imx_bayer_install_proverty2(gobject_class);
}

static void
imx_bayer_2ndhalf_init (GstImxBayer2ndHalf * filter)
{
  imx_bayer_2ndhalf_reset (filter);
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (filter), TRUE);
}

static void
imx_bayer_2ndhalf_set_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED const GValue * value, GParamSpec * pspec)
{
  GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF (object);

  switch (prop_id) {
    case PROP_FBDEV_NUM:
      imxbayer->fb = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
imx_bayer_2ndhalf_get_property (GObject * object, guint prop_id,
    G_GNUC_UNUSED GValue * value, GParamSpec * pspec)
{
  GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF (object);

  switch (prop_id) {
    case PROP_FBDEV_NUM:
      g_value_set_uint(value, imxbayer->fb);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
imx_bayer_2ndhalf_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF (base);
  GstStructure *structure;
  const char *format;
  GstVideoInfo info;

  GST_DEBUG ("in caps %" GST_PTR_FORMAT " out caps %" GST_PTR_FORMAT, incaps,
      outcaps);

  structure = gst_caps_get_structure (incaps, 0);

  gst_structure_get_int (structure, "width", &imxbayer->width);
  gst_structure_get_int (structure, "height", &imxbayer->height);

  structure = gst_caps_get_structure (outcaps, 0);
  format = gst_structure_get_string (structure, "format");
  if (g_str_equal (format, "BGRA")) {
    imxbayer->out_fmt = GST_EGL_TRANS_FORMAT_BGRA;
  } else if (g_str_equal (format, "I420")) {
    imxbayer->out_fmt = GST_EGL_TRANS_FORMAT_I420;
  } else {
    return FALSE;
  }

  if (gst_imx_egl_viv_trans_gles2_fb_setup(imxbayer->fbdata,
												imxbayer->width,
												imxbayer->height,
												imxbayer->out_fmt) == FALSE) {
	  return FALSE;
  }

  /* To cater for different RGB formats, we need to set params for later */
  gst_video_info_from_caps (&info, outcaps);
  imxbayer->info = info;

  return TRUE;
}

static void
imx_bayer_2ndhalf_reset (GstImxBayer2ndHalf * filter)
{
  filter->width = 0;
  filter->height = 0;
  filter->fb = DEFAULT_FBDEV_NUM;

  gst_video_info_init (&filter->info);
}

static GstCaps *
imx_bayer_2ndhalf_transform_caps (G_GNUC_UNUSED GstBaseTransform * base,
    GstPadDirection G_GNUC_UNUSED direction, GstCaps * caps, G_GNUC_UNUSED GstCaps * filter)
{
  GstStructure *structure;
  GstCaps *newcaps;
  GstStructure *newstruct;

  GST_DEBUG_OBJECT (caps, "transforming caps (from)");

  structure = gst_caps_get_structure (caps, 0);

  newcaps = gst_caps_new_empty_simple ("video/x-raw");
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
imx_bayer_2ndhalf_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    gsize * size)
{
  GstStructure *structure;
  int width;
  int height;

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_get_int (structure, "width", &width) &&
      gst_structure_get_int (structure, "height", &height)) {

      *size = width * height * 4;
      return TRUE;
  }
  GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
      ("Incomplete caps, some required field missing"));
  return FALSE;
}

static gboolean imx_bayer_2ndhalf_decide_allocation(G_GNUC_UNUSED GstBaseTransform *transform, GstQuery *query)
{
	return imx_bayer_decide_allocation_base(query);
}

static GstFlowReturn
imx_bayer_2ndhalf_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
	GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF(base);
	gboolean eglstat;
	GstFlowReturn flowstat = GST_FLOW_OK;

	eglstat = gst_imx_egl_viv_trans_gles2_copy_fb(
					imxbayer->fbdata,
					inbuf,
					outbuf);
	if (eglstat == FALSE) {
		flowstat = GST_FLOW_ERROR;
	}

	return flowstat;
}

static gboolean imx_bayer_2ndhalf_start(GstBaseTransform *base)
{
	gboolean ret = TRUE;
	GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF(base);

	imxbayer->fbdata = gst_imx_egl_viv_trans_gles2_fbdata_create(imxbayer->fb);
	if (imxbayer->fbdata == NULL) {
		ret = FALSE;
	}

	return ret;
}

static gboolean imx_bayer_2ndhalf_stop(GstBaseTransform *base)
{
	GstImxBayer2ndHalf *imxbayer = GST_IMXBAYER2ND_HALF(base);

	gst_imx_egl_viv_trans_gles2_fb_deinit(imxbayer->fbdata);
	imxbayer->fbdata = NULL;
	return TRUE;
}
