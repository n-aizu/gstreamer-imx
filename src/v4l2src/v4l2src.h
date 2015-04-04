/*
 * Copyright (c) 2013-2014, Black Moth Technologies
 *   Author: Philip Craig <phil@blackmoth.com.au>
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

#ifndef GST_IMX_V4L2SRC_H
#define GST_IMX_V4L2SRC_H

#include "../common/fd_object.h"

G_BEGIN_DECLS

typedef struct _GstImxV4l2Src GstImxV4l2Src;
typedef struct _GstImxV4l2SrcClass GstImxV4l2SrcClass;

#define GST_TYPE_IMX_V4L2SRC \
	(gst_imx_v4l2src_get_type())
#define GST_IMX_V4L2SRC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_IMX_V4L2SRC, GstImxV4l2Src))
#define GST_IMX_V4L2SRC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_IMX_V4L2SRC, GstImxV4l2SrcClass))
#define GST_IMX_V4L2SRC_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_IMX_V4L2SRC, GstImxV4l2SrcClass))
#define GST_IS_IMX_V4L2SRC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_IMX_V4L2SRC))
#define GST_IS_IMX_V4L2SRC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_IMX_V4L2SRC))

struct _GstImxV4l2Src
{
	GstPushSrc parent;

	GstImxFDObject *fd_obj_v4l;

	gint capture_width;
	gint capture_height;
	guint32 count;
	GstClockTime time_per_frame;

	/* properties */
	gint capture_mode;
	gint capture_format;
	gint fps_n;
	gint fps_d;
	gint input;
	char *devicename;
	int queue_size;
};

struct _GstImxV4l2SrcClass
{
	GstPushSrcClass parent_class;
};

GType gst_imx_v4l2src_get_type(void);

G_END_DECLS

#endif
