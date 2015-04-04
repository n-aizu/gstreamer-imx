#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <linux/ipu.h>

#include "../common/phys_mem_meta.h"
#include "ipu.h"


static GMutex inst_counter_mutex;
static int inst_counter = 0;
static int ipu_fd = -1;


gboolean gst_imx_bayer_ipu_open(void)
{
	g_mutex_lock(&inst_counter_mutex);
	if (inst_counter == 0)
	{
		g_assert(ipu_fd == -1);
		ipu_fd = open("/dev/mxc_ipu", O_RDWR, 0);
		if (ipu_fd < 0)
		{
			GST_ERROR("could not open /dev/mxc_ipu: %s", strerror(errno));
			return FALSE;
		}

		GST_INFO("IPU device opened");
	}
	++inst_counter;
	g_mutex_unlock(&inst_counter_mutex);

	return TRUE;
}


void gst_imx_bayer_ipu_close(void)
{
	g_mutex_lock(&inst_counter_mutex);
	if (inst_counter > 0)
	{
		--inst_counter;
		if (inst_counter == 0)
		{
			g_assert(ipu_fd != -1);
			close(ipu_fd);
			ipu_fd = -1;

			GST_INFO("IPU device closed");
		}
	}
	g_mutex_unlock(&inst_counter_mutex);
}


int gst_imx_bayer_ipu_get_fd(void)
{
	return ipu_fd;
}


gboolean gst_imx_bayer_ipu_yuv_init(int width, int height, unsigned long *ipu_paddr, void **ipu_vaddr)
{
	void *ptr;
	int fd, ret;
	size_t osize;
	dma_addr_t paddr;
	gboolean bret;

	bret = gst_imx_bayer_ipu_open();
	if (bret == FALSE) {
		GST_ERROR("ipu dev open error");
		goto end;
	}

	fd = gst_imx_bayer_ipu_get_fd();

	bret = FALSE;
	osize = width * height * 12 / 8; /* YUV420 */
	paddr = osize;
	ret = ioctl(fd, IPU_ALLOC, &paddr);
	if (ret < 0) {
		GST_ERROR("alloc ioctl error(%s)", strerror(errno));
		goto end;
	}

	ptr = mmap(0, osize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, paddr);
	if (ptr == MAP_FAILED) {
		GST_ERROR("mmap error(%s)", strerror(errno));
		goto end;
	}

	*ipu_paddr = (unsigned long)paddr;
	*ipu_vaddr = ptr;

	bret = TRUE;
end:
	return bret;
}


gboolean gst_imx_bayer_ipu_yuv_end(int width, int height, unsigned long ipu_paddr, void *ipu_vaddr)
{
	int ret, fd;
	size_t osize;
	dma_addr_t paddr;
	gboolean bret = TRUE;

	osize = width * height * 12 / 8;
	ret = munmap(ipu_vaddr, osize);
	if (ret < 0) {
		GST_ERROR("munmap error(%s)", strerror(errno));
		bret = FALSE;
	}

	fd = gst_imx_bayer_ipu_get_fd();

	paddr = (dma_addr_t)ipu_paddr;
	ret = ioctl(fd, IPU_FREE, &paddr);
	if (ret < 0) {
		GST_ERROR("free ioctl error(%s)", strerror(errno));
		bret = FALSE;
	}

	gst_imx_bayer_ipu_close();

	return bret;
}

gboolean gst_imx_bayer_ipu_yuv_conv(int width, int height, unsigned long fb_paddr, unsigned long dst_paddr, void *ipu_vaddr, void *dest)
{
	int ret, fd;
	struct ipu_task task;
	gboolean bret = FALSE;

	fd = gst_imx_bayer_ipu_get_fd();

	memset(&task, 0, sizeof(task));

	task.input.width = width;
	task.input.height = height;
	task.input.format = v4l2_fourcc('B', 'G', 'R', 'A');

	task.output.width = task.input.width;
	task.output.height = task.input.height;
	task.output.format = v4l2_fourcc('Y', 'U', '1', '2');
	task.output.rotate = 0;

	task.input.paddr = (dma_addr_t)fb_paddr;
	task.output.paddr = (dma_addr_t)dst_paddr;

	ret = ioctl(fd, IPU_QUEUE_TASK, &task);
	if (ret < 0) {
		GST_ERROR("queue ioctl error(%s)", strerror(errno));
		goto end;
	}

	if (dest != NULL) {
		memcpy(dest, ipu_vaddr, width * height * 12 / 8);
	}

	bret = TRUE;
end:
	return bret;
}


gboolean gst_imx_bayer_ipu_yuv_conv_div(int width, int height, unsigned long fb_paddr, unsigned long dst_paddr, void *ipu_vaddr, void *dest, int hdiv, int vdiv)
{
	int ret, fd, cnt, cnt2;
	struct ipu_task task;
	gboolean bret = FALSE;

	fd = gst_imx_bayer_ipu_get_fd();

	memset(&task, 0, sizeof(task));

	task.input.width = width;
	task.input.height = height;
	task.input.format = v4l2_fourcc('B', 'G', 'R', 'A');

	task.output.width = task.input.width;
	task.output.height = task.input.height;
	task.output.format = v4l2_fourcc('Y', 'U', '1', '2');
	task.output.rotate = 0;

	task.input.paddr = (dma_addr_t)fb_paddr;
	task.output.paddr = (dma_addr_t)dst_paddr;

	task.input.crop.pos.x = 0;
	task.input.crop.pos.y = 0;
	task.input.crop.w = task.input.width / hdiv;
	task.input.crop.h = task.input.height / vdiv;

	task.output.crop.pos.x = 0;
	task.output.crop.pos.y = 0;
	task.output.crop.w = task.output.width / hdiv;
	task.output.crop.h = task.output.height / vdiv;

	/* BGRA -> YUV420 */
	for (cnt = 0; cnt < vdiv; cnt++) {
		for (cnt2 = 0; cnt2 < hdiv; cnt2++) {
			ret = ioctl(fd, IPU_QUEUE_TASK, &task);
			if (ret < 0) {
				GST_ERROR("queue ioctl error(%s)", strerror(errno));
				goto end;
			}

			task.input.crop.pos.x += task.input.crop.w;
			task.output.crop.pos.x += task.output.crop.w;
		}

		task.input.crop.pos.y += task.input.crop.h;
		task.output.crop.pos.y += task.output.crop.h;

		task.input.crop.pos.x = 0;
		task.output.crop.pos.x = 0;
	}

	if (dest != NULL) {
		memcpy(dest, ipu_vaddr, width * height * 12 / 8);
	}

	bret = TRUE;
end:
	return bret;
}

