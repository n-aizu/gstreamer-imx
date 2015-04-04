/* Allocation functions for physical memory
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


#ifndef GST_IMX_BAYER_IPU_ALLOCATOR_H
#define GST_IMX_BAYER_IPU_ALLOCATOR_H

#include <gst/gst.h>
#include "../common/phys_mem_allocator.h"


G_BEGIN_DECLS


typedef struct _GstImxBayerIpuAllocator GstImxBayerIpuAllocator;
typedef struct _GstImxBayerIpuAllocatorClass GstImxBayerIpuAllocatorClass;


#define GST_TYPE_IMX_BAYER_IPU_ALLOCATOR             (gst_imx_bayer_ipu_allocator_get_type())
#define GST_IMX_BAYER_IPU_ALLOCATOR(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_IMX_BAYER_IPU_ALLOCATOR, GstImxBayerIpuAllocator))
#define GST_IMX_BAYER_IPU_ALLOCATOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_IMX_BAYER_IPU_ALLOCATOR, GstImxBayerIpuAllocatorClass))
#define GST_IS_IMX_BAYER_IPU_ALLOCATOR(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_IMX_BAYER_IPU_ALLOCATOR))
#define GST_IS_IMX_BAYER_IPU_ALLOCATOR_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_IMX_BAYER_IPU_ALLOCATOR))

#define GST_IMX_BAYER_IPU_ALLOCATOR_MEM_TYPE "ImxBayerIpuMemory"


struct _GstImxBayerIpuAllocator
{
	GstImxPhysMemAllocator parent;
};


struct _GstImxBayerIpuAllocatorClass
{
	GstImxPhysMemAllocatorClass parent_class;
};


GType gst_imx_bayer_ipu_allocator_get_type(void);

/* Note that this function returns a floating reference. See gst_object_ref_sink() for details. */
GstAllocator* gst_imx_bayer_ipu_allocator_new(void);
GstBufferPool* gst_imx_bayer_ipu_create_bufferpool(GstCaps *caps, guint size, guint min_buffers, guint max_buffers);


G_END_DECLS


#endif

