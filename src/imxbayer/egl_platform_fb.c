#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include "egl_platform.h"
#include "egl_misc.h"
#include "gl_headers.h"


GST_DEBUG_CATEGORY_STATIC(imx_egl_trans_platform_fb_debug);
#define GST_CAT_DEFAULT imx_egl_trans_platform_fb_debug


struct _GstImxEglVivTransEGLPlatform
{
	EGLNativeDisplayType native_display;
	EGLNativeWindowType native_window;
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLSurface egl_surface;
};


static void init_debug_category(void)
{
	static gboolean initialized = FALSE;
	if (!initialized)
	{
		GST_DEBUG_CATEGORY_INIT(imx_egl_trans_platform_fb_debug, "imxegltransplatform_fb", 0, "imxeglvivtrans FB platform");
		initialized = TRUE;
	}
}




GstImxEglVivTransEGLPlatform* gst_imx_egl_viv_trans_egl_platform_create(gchar const *native_display_name)
{
	gint64 display_index;
	EGLint ver_major, ver_minor;
	GstImxEglVivTransEGLPlatform* platform;

	init_debug_category();

	platform = (GstImxEglVivTransEGLPlatform *)g_new0(GstImxEglVivTransEGLPlatform, 1);

	if (native_display_name == NULL)
		display_index = 0;
	else
		display_index = g_ascii_strtoll(native_display_name, NULL, 10);
	platform->native_display = fbGetDisplayByIndex(display_index);

	platform->egl_display = eglGetDisplay(platform->native_display);
	if (platform->egl_display == EGL_NO_DISPLAY)
	{
		GST_ERROR("eglGetDisplay failed: %s", gst_imx_egl_viv_trans_egl_platform_get_last_error_string());
		goto cleanup;
	}

	if (!eglInitialize(platform->egl_display, &ver_major, &ver_minor))
	{
		GST_ERROR("eglInitialize failed: %s", gst_imx_egl_viv_trans_egl_platform_get_last_error_string());
		goto cleanup;
	}

	GST_INFO("FB EGL platform initialized, using EGL %d.%d", ver_major, ver_minor);

	return platform;


cleanup:
	g_free(platform);
	return NULL;
}


void gst_imx_egl_viv_trans_egl_platform_destroy(GstImxEglVivTransEGLPlatform *platform)
{
	if (platform == NULL)
		return;

	if (platform->egl_display != EGL_NO_DISPLAY)
		eglTerminate(platform->egl_display);

	g_free(platform);
}


gboolean gst_imx_egl_viv_trans_egl_platform_init_window(GstImxEglVivTransEGLPlatform *platform, gint x_coord, gint y_coord, guint width, guint height)
{
	EGLint num_configs;
	EGLConfig config;
	int actual_x, actual_y, actual_width, actual_height;

	static EGLint const eglconfig_attribs[] =
	{
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	static EGLint const ctx_attribs[] =
	{
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	if (!eglChooseConfig(platform->egl_display, eglconfig_attribs, &config, 1, &num_configs))
	{
		GST_ERROR("eglChooseConfig failed: %s", gst_imx_egl_viv_trans_egl_platform_get_last_error_string());
		return FALSE;
	}

	platform->native_window = fbCreateWindow(platform->native_display, x_coord, y_coord, width, height);

	fbGetWindowGeometry(platform->native_window, &actual_x, &actual_y, &actual_width, &actual_height);
	GST_LOG("fbGetWindowGeometry: x/y %d/%d width/height %d/%d", actual_x, actual_y, actual_width, actual_height);

	eglBindAPI(EGL_OPENGL_ES_API);

	platform->egl_context = eglCreateContext(platform->egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
	platform->egl_surface = eglCreateWindowSurface(platform->egl_display, config, platform->native_window, NULL);

	eglMakeCurrent(platform->egl_display, platform->egl_surface, platform->egl_surface, platform->egl_context);

	glViewport(0, 0, actual_width, actual_height);

	return TRUE;
}


gboolean gst_imx_egl_viv_trans_egl_platform_shutdown_window(GstImxEglVivTransEGLPlatform *platform)
{
	if (platform->native_window == 0)
		return TRUE;

	eglMakeCurrent(platform->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (platform->egl_context != EGL_NO_CONTEXT)
		eglDestroyContext(platform->egl_display, platform->egl_context);

	if (platform->egl_surface != EGL_NO_SURFACE)
		eglDestroySurface(platform->egl_display, platform->egl_surface);

	if (platform->egl_display != EGL_NO_DISPLAY)
		eglTerminate(platform->egl_display);

	platform->egl_display = EGL_NO_DISPLAY;
	platform->egl_context = EGL_NO_CONTEXT;
	platform->egl_surface = EGL_NO_SURFACE;
	fbDestroyWindow(platform->native_window);
	platform->native_window = 0;

	return TRUE;
}

void gst_imx_egl_viv_trans_egl_platform_swap_buffers(GstImxEglVivTransEGLPlatform *platform)
{
	eglSwapBuffers(platform->egl_display, platform->egl_surface);
}

