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
#include <sys/time.h>

#include "gl_headers.h"
#include "gles2_renderer.h"
#include "egl_platform.h"
#include "ipu.h"
#include "ipu_allocator.h"
#include "../common/phys_mem_buffer_pool.h"
#include "../common/phys_mem_meta.h"

/* XXX: workaround for build error - include ipu.h here */
#include <linux/ipu.h>


GST_DEBUG_CATEGORY_STATIC(imx_gles2transfer_debug);
#define GST_CAT_DEFAULT imx_gles2transfer_debug


struct _GstImxEglVivTransGLES2Renderer
{
	gboolean init;

	guint in_fmt, out_fmt;
	guint window_width, window_height;
	guint packed_window_width, out_width;
	guint demosaic;
	gboolean hd_lite;
	GstImxEglVivTransEGLPlatform *egl_platform;

	GLuint vertex_shader, fragment_shader, program;
	GLuint vertex_buffer;
	GLuint texture;

	GLint source, source_size, first_red;
	GLint red_coeff, green_coeff, blue_coeff;
	GLint position_aloc, texcoords_aloc;

	gfloat red_coeff_value;
	gfloat green_coeff_value;
	gfloat blue_coeff_value;

	gboolean viv_ext;
	GLvoid* viv_planes[3];

	gchar fb_name[16];
	gchar display_name[8];

	GThread *thread;
	GstPad *push_pad;
	GstAllocator *allocator;
	GstBufferPool *pool;
	GstClockTime pts, dts, duration;
	gboolean phys_alloc;
	gboolean offload;
	gsize out_size;

	gboolean useipu;
	gint hdiv, vdiv;
	guint ipu_in_fmt, ipu_out_fmt;
	size_t ipu_out_size;
	unsigned long ipu_paddr;
	void *ipu_vaddr;
	size_t map_len;
	void* fb_map;
	unsigned long fb_paddr;
};

static GMutex fb_mutex;
static GCond fb_cond_prod, fb_cond_cons;
static volatile gboolean fb_data;
static volatile gboolean thread_end;

static gpointer gst_imx_egl_viv_sink_gles2_fb_thread(gpointer thread_data);
static gboolean gst_imx_egl_viv_trans_mapfb(GstImxEglVivTransGLES2Renderer *renderer, gboolean fbset);
static gboolean gst_imx_egl_viv_trans_alloc_pool(GstImxEglVivTransGLES2Renderer *renderer, gsize out_size, GstCaps *out_caps);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_check_gl_error(char const *category, char const *label, int line);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_shader(GLuint *shader, GLenum shader_type, char const *code);
#if 0
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_shader(GLuint *shader, GLenum shader_type);
#endif
static gboolean gst_imx_egl_viv_trans_gles2_renderer_link_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader);
#if 0
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader);
#endif
static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_vertex_buffer(GLuint *vertex_buffer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_vertex_buffer(GLuint *vertex_buffer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_search_extension(GLubyte const *extensions);

static gboolean gst_imx_egl_viv_trans_gles2_renderer_setup_resources(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_teardown_resources(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_fill_texture(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *buffer);
static GstBuffer *gst_imx_egl_viv_trans_gles2_renderer_acquire_buffer(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_wait_fb_thread(void);
static gboolean gst_imx_egl_viv_trans_gles2_wait_renderdata(void);
static void gst_imx_egl_viv_trans_gles2_finish_render(void);
static void gst_imx_egl_viv_trans_gles2_renderdata_rcv_done(void);
static void gst_imx_egl_viv_trans_gles2_copy_fb(GstImxEglVivTransGLES2Renderer *renderer);
static void gst_imx_egl_viv_trans_gles2_renderer_stop(GstImxEglVivTransGLES2Renderer *renderer);

#define CHECK_GL_ERROR(str1, str2) \
	gst_imx_egl_viv_trans_gles2_renderer_check_gl_error(str1, str2, __LINE__)

/* http://graphics.cs.williams.edu/papers/BayerJGT09/ */
static char const *vert_demosaic_mhc =
	"attribute vec2 a_position;\n"
	"attribute vec2 a_texCoord;\n"

	/** (w,h,1/w,1/h) */
	"uniform vec4   sourceSize;\n"

	/** Pixel position of the first red pixel in the */
	/**  Bayer pattern.  [{0,1}, {0, 1}]*/
	"uniform vec2   firstRed;\n"

	/** .xy = Pixel being sampled in the fragment shader on the range [0, 1]
		.zw = ...on the range [0, sourceSize], offset by firstRed */
	"varying vec4   center;\n"

	/** center.x + (-2/w, -1/w, 1/w, 2/w); These are the x-positions */
	/** of the adjacent pixels.*/
	"varying vec4   xCoord;\n"

	/** center.y + (-2/h, -1/h, 1/h, 2/h); These are the y-positions */
	/** of the adjacent pixels.*/
	"varying vec4   yCoord;\n"

	"void main(void) {\n"
	"    center.xy = a_texCoord;\n"
	"    center.zw = a_texCoord * sourceSize.xy + firstRed;\n"

	"    vec2 invSize = sourceSize.zw;\n"
	"    xCoord = center.x + vec4(-2.0 * invSize.x,\n"
	"        -invSize.x, invSize.x, 2.0 * invSize.x);\n"
	"    yCoord = center.y + vec4(-2.0 * invSize.y,\n"
	"        -invSize.y, invSize.y, 2.0 * invSize.y);\n"

	"    gl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}"
;

static char const *frag_demosaic_mhc =
	/** Monochrome RGBA or GL_LUMINANCE Bayer encoded texture.*/
	"uniform sampler2D source;\n"
	"uniform float red_coeff;\n"
	"uniform float green_coeff;\n"
	"uniform float blue_coeff;\n"
	"varying vec4 center;\n"
	"varying vec4 yCoord;\n"
	"varying vec4 xCoord;\n"

	"void main(void) {\n"
	"    #define fetch(x, y) texture2D(source, vec2(x, y)).a\n"
	"    float C = texture2D(source, center.xy).a; // ( 0, 0)\n"
	"    const vec4 kC = vec4( 4.0,  6.0,  5.0,  5.0) / 8.0;\n"

	/* Determine which of four types of pixels we are on. */
	"    vec2 alternate = mod(floor(center.zw), 2.0);\n"

	"    vec4 Dvec = vec4(\n"
	"        fetch(xCoord[1], yCoord[1]),\n"  /* (-1,-1) */
	"        fetch(xCoord[1], yCoord[2]),\n"  /* (-1, 1) */
	"        fetch(xCoord[2], yCoord[1]),\n"  /* ( 1,-1) */
	"        fetch(xCoord[2], yCoord[2]));\n" /* ( 1, 1) */

	"    vec4 PATTERN = (kC.xyz * C).xyzz;\n"

	/* Can also be a dot product with (1,1,1,1) on hardware where that is */
	/* specially optimized. */
	/* Equivalent to: D = Dvec[0] + Dvec[1] + Dvec[2] + Dvec[3]; */
#if 0
	"    Dvec.xy += Dvec.zw;\n"
	"    Dvec.x  += Dvec.y;\n"
#else
	"    const vec4 dotf = vec4(1.0, 1.0, 1.0, 1.0);\n"
	"    Dvec.x = dot(Dvec, dotf);\n"
#endif

	"    vec4 value = vec4(\n"
	"        fetch(center.x, yCoord[0]),\n"   /* ( 0,-2) */
	"        fetch(center.x, yCoord[1]),\n"   /* ( 0,-1) */
	"        fetch(xCoord[0], center.y),\n"   /* (-1, 0) */
	"        fetch(xCoord[1], center.y));\n"  /* (-2, 0) */

	"    vec4 temp = vec4(\n"
	"        fetch(center.x, yCoord[3]),\n"   /* ( 0, 2) */
	"        fetch(center.x, yCoord[2]),\n"   /* ( 0, 1) */
	"        fetch(xCoord[3], center.y),\n"   /* ( 2, 0) */
	"        fetch(xCoord[2], center.y));\n"  /* ( 1, 0) */

	"    const vec4 kA = vec4(-1.0, -1.5,  0.5, -1.0) / 8.0;\n"
	"    const vec4 kB = vec4( 2.0,  0.0,  0.0,  4.0) / 8.0;\n"
	"    const vec4 kD = vec4( 0.0,  2.0, -1.0, -1.0) / 8.0;\n"

	/* Conserve constant registers and take advantage of free swizzle on load */
	"    #define kE (kA.xywz)\n"
	"    #define kF (kB.xywz)\n"

	"    value += temp;\n"

	/* There are five filter patterns (identity, cross, checker, */
	/* theta, phi).  Precompute the terms from all of them and then */
	/* use swizzles to assign to color channels. */

	/* Channel   Matches */
	/*   x       cross   (e.g., EE G) */
	/*   y       checker (e.g., EE B) */
	/*   z       theta   (e.g., EO R) */
	/*   w       phi     (e.g., EO R) */
	"    #define A (value[0])\n"
	"    #define B (value[1])\n"
	"    #define D (Dvec.x)\n"
	"    #define E (value[2])\n"
	"    #define F (value[3])\n"

	/* Avoid zero elements. On a scalar processor this saves two MADDs */
	/* and it has no effect on a vector processor. */
	"    PATTERN.yzw += (kD.yz * D).xyy;\n"

	"    PATTERN += (kA.xyz * A).xyzx + (kE.xyw * E).xyxz;\n"
	"    PATTERN.xw  += kB.xw * B;\n"
	"    PATTERN.xz  += kF.xz * F;\n"

	/* Avoid conditional move. It is more suitable for vivante GPU. */
#if 0
	"    vec3 tmp_rgb = (alternate.y == 0.0) ?\n"
	"        ((alternate.x == 0.0) ?\n"
	"            vec3(C, PATTERN.xy) :\n"
	"            vec3(PATTERN.z, C, PATTERN.w)) :\n"
	"        ((alternate.x == 0.0) ?\n"
	"            vec3(PATTERN.w, C, PATTERN.z) :\n"
	"            vec3(PATTERN.yx, C));\n"

	"    vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"    gl_FragColor.rgb = tmp_rgb * coeff;\n"
#else
	"    vec3 rb = vec3(C, PATTERN.xy);\n"
	"    vec3 gg = vec3(PATTERN.w, C, PATTERN.z);\n"
	"    vec3 temp2 = mix(rb, gg, distance(alternate.x, alternate.y));\n"
	"    vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"    gl_FragColor.rgb = mix(temp2.xyz, temp2.zyx, step(0.5, alternate.x)) * coeff;\n"
#endif
	"}"
;


/* http://techtidings.blogspot.jp/2012/01/demosaicing-exposed-normal-edge-aware.html */
static char const *vert_demosaic_edge =
	"attribute vec3 a_position;\n"
	"attribute vec2 a_texCoord;\n"
	/** (w,h,1/w,1/h) */
	"uniform vec4   sourceSize;\n"

	/** Pixel position of the first red pixel in the */
	/**  Bayer pattern.  [{0,1}, {0, 1}]*/
	"uniform vec2   firstRed;\n"

	/** .xy = Pixel being sampled in the fragment shader on the range [0, 1]
		.zw = ...on the range [0, sourceSize], offset by firstRed */
	"varying vec4   center;\n"

	"varying vec2   xCoord;\n"
	"varying vec2   yCoord;\n"

	"void main(void) {\n"
	"	center.xy = a_texCoord;\n"
	"	center.zw = a_texCoord * sourceSize.xy + firstRed;\n"

	"	vec2 invSize = sourceSize.zw;\n"
	"	xCoord = center.x + (vec2(-1.0, 1.0) * invSize.x);\n"
	"	yCoord = center.y + (vec2(-1.0, 1.0) * invSize.y);\n"

	"	gl_Position = vec4(a_position, 1.0);\n"
	"}"
;

static char const *frag_demosaic_edge =
	"uniform sampler2D source;\n"
	"varying vec4 center;\n"
	"varying vec2 xCoord;\n"
	"varying vec2 yCoord;\n"
	"uniform float red_coeff;\n"
	"uniform float green_coeff;\n"
	"uniform float blue_coeff;\n"

	"void main(void) {\n"
	"	#define fetch(x, y) texture2D(source, vec2(x, y)).a\n"
	"	float C = texture2D(source, center.xy).a; // ( 0, 0)\n"

	/* Determine which of four types of pixels we are on. */
	"	vec2 alternate = mod(floor(center.zw), 2.0);\n"

	"	vec4 value1 = vec4(\n"
	"		fetch(center.x, yCoord[0]),\n"   /* ( 0,-1) */
	"		fetch(center.x, yCoord[1]),\n"   /* ( 0, 1) */
	"		fetch(xCoord[0], center.y),\n"   /* (-1, 0) */
	"		fetch(xCoord[1], center.y));\n"  /* ( 1, 0) */

	"	vec4 value2 = vec4(\n"
	"		fetch(xCoord[0], yCoord[0]),\n"  /* (-1,-1) */
	"		fetch(xCoord[0], yCoord[1]),\n"  /* (-1, 1) */
	"		fetch(xCoord[1], yCoord[0]),\n"  /* ( 1,-1) */
	"		fetch(xCoord[1], yCoord[1]));\n" /* ( 1, 1) */

	"	vec4 v4tmp1 = vec4(value1.xz, value2.xz);\n"
	"	vec4 v4tmp2 = vec4(value1.yw, value2.yw);\n"

	"	vec4 hvsumm = (v4tmp1 + v4tmp2) * 0.5;\n"
	"	vec4 hvdiff = max(abs(v4tmp1 - v4tmp2), vec4(1.0, 1.0, 1.0, 1.0) / 256.0);\n"

	/* calculate green value */
	"	float tmp = dot(hvsumm.xy, hvdiff.yx);\n"
	"	float edge1 = tmp / (hvdiff.x + hvdiff.y);\n"

	/* calculate red/blue value */
	"	tmp = dot(hvsumm.zw, hvdiff.wz);\n"
	"	float edge2 = tmp / (hvdiff.z + hvdiff.w);\n"

	"   vec3 rb = vec3(C, edge1, edge2);\n"
	"   vec3 gg = vec3(hvsumm.x, C, hvsumm.y);\n"
	"   vec3 temp2 = mix(rb, gg, distance(alternate.x, alternate.y));\n"
	"	vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"	gl_FragColor.rgb = mix(temp2.xyz, temp2.zyx, step(0.5, alternate.x)) * coeff;\n"
	"}"
;


static char const *vert_demosaic_edge_yuv =
	"attribute vec3 a_position;\n"
	"attribute vec2 a_texCoord;\n"
	/** (w,h,1/w,1/h) */
	"uniform vec4   sourceSize;\n"

	/** Pixel position of the first red pixel in the */
	/**  Bayer pattern.  [{0,1}, {0, 1}]*/
	"uniform vec2   firstRed;\n"

	/** .xy = Pixel being sampled in the fragment shader on the range [0, 1]
		.zw = ...on the range [0, sourceSize], offset by firstRed */
	"varying vec4   center;\n"

	"varying vec4   xCoord;\n"
	"varying vec2   yCoord;\n"

	"void main(void) {\n"
	"	center.x = a_texCoord.x + ((1.0 / (sourceSize.x - 1.0)) / 2.0);\n"
	"	center.y = a_texCoord.y;\n"
	"	center.zw = center.xy * sourceSize.xy + firstRed;\n"

	"	vec2 invSize = sourceSize.zw;\n"
	"	xCoord = center.x + (vec4(-1.0, 0.0, 1.0, 2.0) * invSize.x);\n"
	"	yCoord = center.y + (vec2(-1.0, 1.0) * invSize.y);\n"
	"	gl_Position = vec4(a_position, 1.0);\n"
	"}"
;

static char const *vert_demosaic_edge_yuv_lite =
	"attribute vec3 a_position;\n"
	"attribute vec2 a_texCoord;\n"
	/** (w,h,1/w,1/h) */
	"uniform vec4   sourceSize;\n"

	/** Pixel position of the first red pixel in the */
	/**  Bayer pattern.  [{0,1}, {0, 1}]*/
	"uniform vec2   firstRed;\n"

	/** .xy = Pixel being sampled in the fragment shader on the range [0, 1]
		.zw = ...on the range [0, sourceSize], offset by firstRed */
	"varying vec4   center;\n"

	"varying vec4   xCoord;\n"
	"varying vec2   yCoord;\n"

	"void main(void) {\n"
	"	center.xy = a_texCoord;\n"
	"	center.zw = a_texCoord * sourceSize.xy + firstRed;\n"

	"	vec2 invSize = sourceSize.zw;\n"
	"	xCoord = center.x + (vec4(-1.0, 0.0, 1.0, 2.0) * invSize.x);\n"
	"	yCoord = center.y + (vec2(-1.0, 1.0) * invSize.y);\n"
	"	gl_Position = vec4(a_position, 1.0);\n"
	"}"
;

static char const *frag_demosaic_edge_yuv =
	"uniform sampler2D source;\n"
	"varying vec4 center;\n"
	"varying vec4 xCoord;\n"
	"varying vec2 yCoord;\n"
	"uniform float red_coeff;\n"
	"uniform float green_coeff;\n"
	"uniform float blue_coeff;\n"

	"void main(void) {\n"
	"	#define fetch(x, y) texture2D(source, vec2(x, y)).a\n"

	/* Determine which of four types of pixels we are on. */
	"	vec2 alternate = mod(floor(center.zw), 2.0);\n"
	"	float flag = distance(alternate.x, alternate.y);\n"
	"	vec4 xpos = mix(xCoord, xCoord.wzyx, flag);\n"

	"	vec4 value1 = vec4(\n"
	"		fetch(xpos[1], yCoord[0]),\n"
	"		fetch(xpos[1], yCoord[1]),\n"
	"		fetch(xpos[0], center.y),\n"
	"		fetch(xpos[2], center.y));\n"

	"	vec4 value2 = vec4(\n"
	"		fetch(xpos[2], yCoord[0]),\n"
	"		fetch(xpos[2], yCoord[1]),\n"
	"		fetch(xpos[0], yCoord[0]),\n"
	"		fetch(xpos[0], yCoord[1]));\n"

	"	vec2 value3 = vec2(\n"
	"		fetch(xpos[1], center.y),\n"
	"		fetch(xpos[3], center.y));\n"

	/* red/blue pixel */
	"	#define C (value3.x)\n"

	"	vec4 v4tmp1 = vec4(value1.xz, value2.xz);\n"
	"	vec4 v4tmp2 = vec4(value1.yw, value2.wy);\n"

	"	vec4 hvsumm = (v4tmp1 + v4tmp2) * 0.5;\n"
	"	vec4 hvdiff = max(abs(v4tmp1 - v4tmp2), vec4(1.0, 1.0, 1.0, 1.0) / 256.0);\n"

	/* calculate green value */
	"	float tmp = dot(hvsumm.xy, hvdiff.yx);\n"
	"	float green1 = tmp / (hvdiff.x + hvdiff.y);\n"
	"	vec2 gg = vec2(green1, value1.w);\n"

	/* calculate red/blue value */
	"	tmp = dot(hvsumm.zw, hvdiff.wz);\n"
	"	vec2 rb1 = vec2(C, tmp / (hvdiff.z + hvdiff.w));\n"
	"	vec2 rb2 = (vec2(value2.x, value3.x) + vec2(value2.y, value3.y)) * 0.5;\n"
	"	vec4 rbrb = mix(vec4(rb1, rb2), vec4(rb1.yx, rb2.yx), step(0.5, alternate.y));\n"

	"	vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"	vec3 rgb_tmp1 = vec3(rbrb.x, gg.x, rbrb.y) * coeff;\n"
	"	vec3 rgb_tmp2 = vec3(rbrb.w, gg.y, rbrb.z) * coeff;\n"

	"	vec3 rgb1 = mix(rgb_tmp1, rgb_tmp2, flag);\n"
	"	vec3 rgb2 = mix(rgb_tmp2, rgb_tmp1, flag);\n"

	/* calculate yuv value */
	/* these formulae are borrowed from glcolorconvert(gst-plugins-bad)  */
	/* these are same as Linux-imx IPU driver(drivers/mxc/ipu3/ipu_ic.c) */
	"	const vec4 offset = vec4(0.5, 0.0625, 0.5, 0.0625);\n"
	"	const vec3 ycoeff = vec3(0.256816, 0.504154, 0.0979137);\n"
	"	const vec3 ucoeff = vec3(-0.148246, -0.29102, 0.439266);\n"
	"	const vec3 vcoeff = vec3(0.439271, -0.367833, -0.071438);\n"

	"	vec4 uyvy;\n"
    "	uyvy.x = dot(rgb1, ucoeff);\n"
    "	uyvy.y = dot(rgb1, ycoeff);\n"
    "	uyvy.z = dot(rgb1, vcoeff);\n"
    "	uyvy.w = dot(rgb2, ycoeff);\n"

	"	gl_FragColor.bgra = uyvy + offset;\n"
	"}"
;


static char const *frag_demosaic_edge_yuv_less =
	"uniform sampler2D source;\n"
	"varying vec4 center;\n"
	"varying vec4 xCoord;\n"
	"varying vec2 yCoord;\n"
	"uniform float red_coeff;\n"
	"uniform float green_coeff;\n"
	"uniform float blue_coeff;\n"

	"void main(void) {\n"
	"	#define fetch(x, y) texture2D(source, vec2(x, y)).a\n"

	/* Determine which of four types of pixels we are on. */
	"	vec2 alternate = mod(floor(center.zw), 2.0);\n"
	"	float flag = distance(alternate.x, alternate.y);\n"
	"	vec3 xpos = mix(xCoord.xyz, xCoord.wzy, flag);\n"

	"	vec4 value1 = vec4(\n"
	"		fetch(xpos[1], yCoord[0]),\n"
	"		fetch(xpos[1], yCoord[1]),\n"
	"		fetch(xpos[0], center.y),\n"
	"		fetch(xpos[2], center.y));\n"

	"	vec2 value2 = vec2(\n"
	"		fetch(xpos[2], yCoord[0]),\n"
	"		fetch(xpos[2], yCoord[1]));\n"

	"	float C = fetch(xpos[1], center.y);\n"

	"	vec3 hvhsumm = (vec3(value1.xz, value2.x) + vec3(value1.yw, value2.y)) * 0.5;\n"
	"	vec2 hvdiff = max(abs(value1.xz - value1.yw), vec2(1.0, 1.0) / 256.0);\n"

	/* calculate green value */
	"	float temp = dot(hvhsumm.xy, hvdiff.yx);\n"
	"	float green1 = temp / (hvdiff.x + hvdiff.y);\n"
	"	vec2 gg = vec2(green1, value1.w);\n"

	/* calculate red/blue value */
	"	vec2 rb1 = vec2(C, hvhsumm.z);\n"
	"	vec2 rb2 = (rb1 - gg) + gg.yx;\n"

	"	vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"	vec4 rrbb = vec4(rb1.x, rb2.x, rb2.y, rb1.y);\n"
	"	rrbb = mix(rrbb, rrbb.zwxy, step(0.5, alternate.y));\n"
	"	vec3 rgb_tmp1 = vec3(rrbb.x, gg.x, rrbb.z) * coeff;\n"
	"	vec3 rgb_tmp2 = vec3(rrbb.y, gg.y, rrbb.w) * coeff;\n"

	"	vec3 rgb1 = mix(rgb_tmp1, rgb_tmp2, flag);\n"
	"	vec3 rgb2 = mix(rgb_tmp2, rgb_tmp1, flag);\n"

	/* calculate yuv value */
	"	const vec4 offset = vec4(0.5, 0.0625, 0.5, 0.0625);\n"
	"	const vec3 ycoeff = vec3(0.256816, 0.504154, 0.0979137);\n"
	"	const vec3 ucoeff = vec3(-0.148246, -0.29102, 0.439266);\n"
	"	const vec3 vcoeff = vec3(0.439271, -0.367833, -0.071438);\n"

	"	vec4 uyvy;\n"
    "	uyvy.x = dot(rgb1, ucoeff);\n"
    "	uyvy.y = dot(rgb1, ycoeff);\n"
    "	uyvy.z = dot(rgb1, vcoeff);\n"
    "	uyvy.w = dot(rgb2, ycoeff);\n"

	"	gl_FragColor.bgra = uyvy + offset;\n"
	"}"
;


static GLfloat const vertex_data[] = {
	-1.0f,  1.0f, /* Position 0 */
	0.0f,  0.0f,  /* TexCoord 0 */
	-1.0f, -1.0f, /* Position 1 */
	0.0f,  1.0f,  /* TexCoord 1 */
	1.0f, -1.0f,  /* Position 2 */
	1.0f,  1.0f,  /* TexCoord 2 */
	1.0f,  1.0f,  /* Position 3 */
	1.0f,  0.0f   /* TexCoord 3 */
};
static unsigned int const vertex_data_size = sizeof(GLfloat)*16;
static unsigned int const vertex_size = sizeof(GLfloat)*4;
static unsigned int const vertex_position_num = 2;
static unsigned int const vertex_position_offset = sizeof(GLfloat)*0;
static unsigned int const vertex_texcoords_num = 2;
static unsigned int const vertex_texcoords_offset = sizeof(GLfloat)*2;

#ifdef DEBUG
static unsigned long long gethrtime(void)
{
	unsigned long long now;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	now = ((unsigned long long)tv.tv_sec * 1000000) + tv.tv_usec;

	return now;
}
#endif

static void init_debug_category(void)
{
	static gboolean initialized = FALSE;
	if (!initialized)
	{
		GST_DEBUG_CATEGORY_INIT(imx_gles2transfer_debug, "imxgles2transfer", 0, "imxeglvivtrans OpenGL ES 2 videotranser renderer");
		initialized = TRUE;
	}
}

static gpointer gst_imx_egl_viv_sink_gles2_fb_thread(gpointer thread_data)
{
	GstImxEglVivTransGLES2Renderer *renderer = (GstImxEglVivTransGLES2Renderer *)thread_data;
	gboolean ret;

	if (renderer->offload != FALSE) {
		ret = gst_imx_egl_viv_trans_egl_platform_create_subcontext(renderer->egl_platform);
		if (ret == FALSE)
			GST_ERROR("create egl context failed");
	}

	while (thread_end == FALSE) {
		gst_imx_egl_viv_trans_gles2_copy_fb(renderer);
	}

	return NULL;
}

static gboolean gst_imx_egl_viv_trans_mapfb(GstImxEglVivTransGLES2Renderer *renderer, gboolean fbset)
{
	int ret, fd;
	void *ptr;
	size_t len;
	struct fb_fix_screeninfo finfo;
	gboolean bret = FALSE;

	fd = open(renderer->fb_name, O_RDONLY);
	if (fd < 0) {
		GST_ERROR("open error(%s)", strerror(errno));
		goto end;
	}

	if (fbset) {
		int width, height;
		struct fb_var_screeninfo var;

		width = renderer->packed_window_width;
		height = renderer->window_height;

		ret = ioctl(fd, FBIOGET_VSCREENINFO, &var);
		if (ret == 0) {
			var.xres = width;
			var.yres = height;
			var.xres_virtual = width;
			var.yres_virtual = height;
			var.bits_per_pixel = 32;
			ret = ioctl(fd, FBIOPUT_VSCREENINFO, &var);
		}

		if (ret < 0) {
			GST_ERROR("ioctl error(%s)", strerror(errno));
			goto close_end;
		}
	}

	ret = ioctl(fd, FBIOGET_FSCREENINFO, &finfo);
	if (ret < 0) {
		GST_ERROR("ioctl error(%s)", strerror(errno));
		goto close_end;
	}

	renderer->fb_paddr = finfo.smem_start;

	len = (renderer->packed_window_width) * renderer->window_height * 4;
	ptr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ptr == MAP_FAILED) {
		GST_ERROR("mmap error(%s)", strerror(errno));
		goto close_end;
	}

	renderer->fb_map = ptr;
	renderer->map_len = len;
	bret = TRUE;

close_end:
	close(fd);
end:
	return bret;
}

static gboolean gst_imx_egl_viv_trans_alloc_pool(GstImxEglVivTransGLES2Renderer *renderer, gsize out_size, GstCaps *out_caps)
{
	GstAllocator *allocator;
	GstBufferPool *pool;
	GstStructure *config;

	allocator = gst_imx_bayer_ipu_allocator_new();
	if (allocator == NULL)
	{
		GST_ERROR("could not create physical memory bufferpool allocator");
		return FALSE;
	}

	renderer->allocator = gst_object_ref_sink(allocator);

	pool = gst_imx_phys_mem_buffer_pool_new(FALSE);
	if (pool == NULL)
	{
		GST_ERROR("could not create physical memory bufferpool");
		gst_object_unref(renderer->allocator);
		return FALSE;
	}

	config = gst_buffer_pool_get_config(pool);
	gst_buffer_pool_config_set_params(config, out_caps, out_size, 4, 0); /* add margin */
	gst_buffer_pool_config_set_allocator(config, renderer->allocator, NULL);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
	gst_buffer_pool_set_config(pool, config);

	if (!gst_buffer_pool_is_active(pool))
		gst_buffer_pool_set_active(pool, TRUE);

	renderer->pool = pool;

	return TRUE;
}

gboolean gst_imx_egl_viv_trans_gles2_renderer_setup(GstImxEglVivTransGLES2Renderer *renderer, GstPad *push_pad, GstCaps *out_caps, guint width, guint height, guint in_fmt, guint out_fmt, gfloat red_coeff, gfloat green_coeff, gfloat blue_coeff, gboolean fbset, gboolean phys_alloc, gboolean hd_lite, guint demosaic)
{
	GLubyte const *extensions;
	GError *error;

	if (renderer->init != FALSE)
	{
		return TRUE;
	}

	renderer->window_width = width;
	renderer->window_height = height;

	if (hd_lite == FALSE || (demosaic != GST_DEMOSAIC_EDGE_YUV && demosaic != GST_DEMOSAIC_EDGE_YUV_LESSER))
	{
		renderer->out_width = width;
		renderer->hd_lite = FALSE;
	}
	else
	{
		renderer->out_width = width * 2 / 3;
		renderer->hd_lite = TRUE;
	}

	if (demosaic == GST_DEMOSAIC_EDGE_YUV ||
		demosaic == GST_DEMOSAIC_EDGE_YUV_LESSER)
	{
		renderer->packed_window_width = renderer->out_width / 2;
		renderer->useipu = (out_fmt != GST_EGL_TRANS_FORMAT_UYVY);
	}
	else
	{
		renderer->packed_window_width = width;
		renderer->useipu = (out_fmt != GST_EGL_TRANS_FORMAT_BGRA);
	}

	renderer->demosaic = demosaic;
	renderer->in_fmt = in_fmt;
	renderer->out_fmt = out_fmt;
	renderer->phys_alloc = phys_alloc;
	renderer->offload = TRUE; /* always TRUE */
	renderer->red_coeff_value = red_coeff;
	renderer->green_coeff_value = green_coeff;
	renderer->blue_coeff_value = blue_coeff;

	if (out_fmt == GST_EGL_TRANS_FORMAT_BGRA)
	{
		renderer->out_size = renderer->out_width * height * 4;
	}
	else if (out_fmt == GST_EGL_TRANS_FORMAT_UYVY)
	{
		renderer->out_size = renderer->out_width * height * 2;
	}
	else
	{
		renderer->out_size = renderer->out_width * height * 12 / 8;
	}

	if (!gst_imx_egl_viv_trans_mapfb(renderer, fbset))
	{
		GST_ERROR("framebuffer mmap error");
		return FALSE;
	}

	if (renderer->useipu)
	{
		if (!gst_imx_bayer_ipu_yuv_init(renderer->out_size,
										&renderer->ipu_paddr,
										&renderer->ipu_vaddr))
		{
			GST_ERROR("ipu init error");
			return FALSE;
		}

		if (renderer->out_width > 1024 ||
			renderer->window_height > 1024) {
			gint hdiv, vdiv;

			if (renderer->window_height >= 1080) {
				vdiv = 3;
			}
			else if (renderer->window_height > 1024) {
				vdiv = 2;
			}
			else {
				vdiv = 1;
			}

			if (renderer->out_width > 1024) {
				hdiv = 2;
			}
			else {
				hdiv = 1;
			}

			renderer->hdiv = hdiv;
			renderer->vdiv = vdiv;
		}

		if (demosaic == GST_DEMOSAIC_EDGE_YUV ||
			demosaic == GST_DEMOSAIC_EDGE_YUV_LESSER)
		{
			renderer->ipu_in_fmt = IPU_PIX_FMT_UYVY;
		}
		else
		{
			renderer->ipu_in_fmt = IPU_PIX_FMT_BGR32;
		}

		if (out_fmt == GST_EGL_TRANS_FORMAT_BGRA)
		{
			renderer->ipu_out_fmt = IPU_PIX_FMT_BGR32;
			renderer->ipu_out_size = width * height * 4;
		}
		else if (out_fmt == GST_EGL_TRANS_FORMAT_UYVY)
		{
			renderer->ipu_out_fmt = IPU_PIX_FMT_UYVY;
			renderer->ipu_out_size = width * height * 2;
		}
		else
		{
			renderer->ipu_out_fmt = IPU_PIX_FMT_YUV420P;
			renderer->ipu_out_size = width * height * 12 / 8;
		}
	}

	renderer->egl_platform = gst_imx_egl_viv_trans_egl_platform_create(
		renderer->display_name
	);
	if (renderer->egl_platform == NULL)
	{
		GST_ERROR("egl platform create failed");
		return FALSE;
	}

	if (!gst_imx_egl_viv_trans_egl_platform_init_window(
		renderer->egl_platform,
		0, 0,
		renderer->packed_window_width, renderer->window_height
	))
	{
		GST_ERROR("could not open window");
		return FALSE;
	}

	extensions = glGetString(GL_EXTENSIONS);
	if (extensions == NULL)
	{
		GST_WARNING("OpenGL ES extension string is NULL");
		renderer->viv_ext = FALSE;
	}

	if (renderer->viv_ext)
	{
		if (gst_imx_egl_viv_trans_gles2_renderer_search_extension(extensions))
			GST_INFO("Vivante direct texture extension (GL_VIV_direct_texture) present");
		else
		{
			GST_WARNING("Vivante direct texture extension (GL_VIV_direct_texture) missing");
			renderer->viv_ext = FALSE;
		}
	}

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	if (!gst_imx_egl_viv_trans_gles2_renderer_setup_resources(renderer))
	{
		GST_ERROR("setting up resources failed");
		return FALSE;
	}

	glBindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer);
	glBindTexture(GL_TEXTURE_2D, renderer->texture);

	if (renderer->phys_alloc != FALSE)
	{
		if (!gst_imx_egl_viv_trans_alloc_pool(renderer, renderer->out_size, out_caps))
		{
			GST_ERROR("setting up buffer pool failed");
			return FALSE;
		}
	}

	renderer->push_pad = gst_object_ref(push_pad);

	renderer->thread = g_thread_try_new("ipu-thread", gst_imx_egl_viv_sink_gles2_fb_thread, renderer, &error);
	if (renderer->thread == NULL)
	{
		if ((error != NULL) && (error->message != NULL))
			GST_ERROR("could not start thread: %s", error->message);
		else
			GST_ERROR("could not start thread: unknown error");

		if (error != NULL)
			g_error_free(error);

		return FALSE;
	}

	renderer->init = TRUE;

	GST_INFO("starting GLES2 renderer");
	return TRUE;
}


static void gst_imx_egl_viv_trans_gles2_renderer_stop(GstImxEglVivTransGLES2Renderer *renderer)
{
	if (!gst_imx_egl_viv_trans_gles2_renderer_teardown_resources(renderer))
		GST_ERROR("tearing down resources failed");

	if (!gst_imx_egl_viv_trans_egl_platform_shutdown_window(renderer->egl_platform))
		GST_ERROR("could not close window");

	if (renderer->useipu)
	{
		if (!gst_imx_bayer_ipu_yuv_end(renderer->out_size,
										renderer->ipu_paddr,
										renderer->ipu_vaddr))
			GST_ERROR("IPU end error");
	}

	munmap(renderer->fb_map, renderer->map_len);
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_check_gl_error(char const *category, char const *label, int line)
{
	GLenum err = glGetError();
	if (err == GL_NO_ERROR)
		return TRUE;

	switch (err)
	{
		case GL_INVALID_ENUM:                  GST_ERROR("(%d) [%s] [%s] error: invalid enum", line, category, label); break;
		case GL_INVALID_VALUE:                 GST_ERROR("(%d) [%s] [%s] error: invalid value", line, category, label); break;
		case GL_INVALID_OPERATION:             GST_ERROR("(%d) [%s] [%s] error: invalid operation", line, category, label); break;
		case GL_INVALID_FRAMEBUFFER_OPERATION: GST_ERROR("(%d) [%s] [%s] error: invalid framebuffer operation", line, category, label); break;
		case GL_OUT_OF_MEMORY:                 GST_ERROR("(%d) [%s] [%s] error: out of memory", line, category, label); break;
		default:                               GST_ERROR("(%d) [%s] [%s] error: unknown GL error 0x%x", line, category, label, err);
	}

	return FALSE;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_shader(GLuint *shader, GLenum shader_type, char const *code)
{
	GLint compilation_status, info_log_length;
	GLchar *info_log;
	char const *shader_type_name;

	switch (shader_type)
	{
		case GL_VERTEX_SHADER: shader_type_name = "vertex shader"; break;
		case GL_FRAGMENT_SHADER: shader_type_name = "fragment shader"; break;
		default:
			GST_ERROR("unknown shader type 0x%x", shader_type);
			return FALSE;
	}

	glGetError(); /* clear out any existing error */

	*shader = glCreateShader(shader_type);
	if (!CHECK_GL_ERROR(shader_type_name, "glCreateShader"))
		return FALSE;

	glShaderSource(*shader, 1, &code, NULL);
	if (!CHECK_GL_ERROR(shader_type_name, "glShaderSource"))
		return FALSE;

	glCompileShader(*shader);
	if (!CHECK_GL_ERROR(shader_type_name, "glCompileShader"))
		return FALSE;

	glGetShaderiv(*shader, GL_COMPILE_STATUS, &compilation_status);
	if (compilation_status == GL_FALSE)
	{
		GST_ERROR("compiling %s failed", shader_type_name);
		glGetShaderiv(*shader, GL_INFO_LOG_LENGTH, &info_log_length);
		info_log = g_new0(GLchar, info_log_length);
		glGetShaderInfoLog(*shader, info_log_length, NULL, info_log);
		GST_INFO("compilation log:\n%s", info_log);
		g_free(info_log);
		return FALSE;
	}
	else
		GST_LOG("successfully compiled %s", shader_type_name);

	return TRUE;
}

#if 0
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_shader(GLuint *shader, GLenum shader_type)
{
	char const *shader_type_name;

	if ((*shader) == 0)
		return TRUE;

	switch (shader_type)
	{
		case GL_VERTEX_SHADER: shader_type_name = "vertex shader"; break;
		case GL_FRAGMENT_SHADER: shader_type_name = "fragment shader"; break;
		default:
			GST_ERROR("unknown shader type 0x%x", shader_type);
			return FALSE;
	}

	glGetError(); /* clear out any existing error */

	glDeleteShader(*shader);
	*shader = 0;
	if (!CHECK_GL_ERROR(shader_type_name, "glDeleteShader"))
		return FALSE;

	return TRUE;
}
#endif

static gboolean gst_imx_egl_viv_trans_gles2_renderer_link_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader)
{
	GLint link_status, info_log_length;
	GLchar *info_log;

	glGetError(); /* clear out any existing error */

	*program = glCreateProgram();
	if (!CHECK_GL_ERROR("program", "glCreateProgram"))
		return FALSE;

	glAttachShader(*program, vertex_shader);
	if (!CHECK_GL_ERROR("program vertex", "glAttachShader"))
		return FALSE;

	glAttachShader(*program, fragment_shader);
	if (!CHECK_GL_ERROR("program fragment", "glAttachShader"))
		return FALSE;

	glLinkProgram(*program);
	if (!CHECK_GL_ERROR("program", "glLinkProgram"))
		return FALSE;

	glGetProgramiv(*program, GL_LINK_STATUS, &link_status);
	if (link_status == GL_FALSE)
	{
		GST_ERROR("linking program failed");
		glGetProgramiv(*program, GL_INFO_LOG_LENGTH, &info_log_length);
		info_log = g_new0(GLchar, info_log_length);
		glGetProgramInfoLog(*program, info_log_length, NULL, info_log);
		GST_INFO("linker log:\n%s", info_log);
		g_free(info_log);
		return FALSE;
	}
	else
		GST_LOG("successfully linked program");

	glUseProgram(*program);

	return TRUE;
}

#if 0
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader)
{
	if ((*program) == 0)
		return TRUE;

	glGetError(); /* clear out any existing error */

	glUseProgram(0);
	if (!CHECK_GL_ERROR("program", "glUseProgram"))
		return FALSE;

	glDetachShader(*program, vertex_shader);
	if (!CHECK_GL_ERROR("program vertex", "glDetachShader"))
		return FALSE;

	glDetachShader(*program, fragment_shader);
	if (!CHECK_GL_ERROR("program fragment", "glDetachShader"))
		return FALSE;

	glDeleteProgram(*program);
	*program = 0;
	if (!CHECK_GL_ERROR("program", "glDeleteProgram"))
		return FALSE;

	return TRUE;
}
#endif

static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_vertex_buffer(GLuint *vertex_buffer)
{
	glGetError(); /* clear out any existing error */

	glGenBuffers(1, vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, *vertex_buffer);
	/* TODO: This has to be called twice, otherwise the vertex data gets corrupted after the first few
	 * rendered frames. Is this a Vivante driver bug? */
	glBufferData(GL_ARRAY_BUFFER, vertex_data_size, vertex_data, GL_STATIC_DRAW);
	glBufferData(GL_ARRAY_BUFFER, vertex_data_size, vertex_data, GL_STATIC_DRAW);
	if (!CHECK_GL_ERROR("vertex buffer", "glBufferData"))
		return FALSE;

	return TRUE;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_vertex_buffer(GLuint *vertex_buffer)
{
	glGetError(); /* clear out any existing error */

	if ((*vertex_buffer) != 0)
	{
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteBuffers(1, vertex_buffer);
		*vertex_buffer = 0;
	}

	return TRUE;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_search_extension(GLubyte const *extensions)
{
	char *buf = NULL;
	int buf_len = 0;
	char const *start, *end;
	start = end = (char const *)extensions;
	gboolean viv_direct_ext_found = FALSE;

	/* go through the space-separated extension list */

	while (1)
	{
		if ((*end == ' ') || (*end == 0))
		{
			if (start != end)
			{
				int token_len = end - start; /* string: [start, end-1] */

				/* enlarge token buffer if it is too small */
				if (buf_len < token_len)
				{
					char *new_buf = realloc(buf, token_len + 1);
					if (new_buf == NULL)
					{
						if (buf != NULL)
							free(buf);
						GST_ERROR("could not (re)allocate %d bytes for token buffer", token_len);
						return FALSE;
					}
					buf = new_buf;
					buf_len = token_len;
				}

				/* copy token to buffer, and add null terminator */
				memcpy(buf, start, token_len);
				buf[token_len] = 0;

				GST_LOG("found extension: %s", buf);

				/* this sink needs direct texture extension is necessary for playback */
				if ((strcmp("GL_VIV_direct_texture", buf) == 0) || (strcmp("GL_VIV_tex_direct", buf) == 0))
					viv_direct_ext_found = TRUE;
			}

			start = end + 1;
		}

		if (*end == 0)
			break;

		++end;
	}

	if (buf != NULL)
		free(buf);

	return viv_direct_ext_found;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_setup_resources(GstImxEglVivTransGLES2Renderer *renderer)
{
	char const *vert;
	char const *frag;

	switch (renderer->demosaic) {
	case GST_DEMOSAIC_MHC:
		vert = vert_demosaic_mhc;
		frag = frag_demosaic_mhc;
		break;

	case GST_DEMOSAIC_EDGE:
		vert = vert_demosaic_edge;
		frag = frag_demosaic_edge;
		break;

	case GST_DEMOSAIC_EDGE_YUV:
		if (renderer->hd_lite == FALSE)
			vert = vert_demosaic_edge_yuv;
		else
			vert = vert_demosaic_edge_yuv_lite;

		frag = frag_demosaic_edge_yuv;
		break;

	case GST_DEMOSAIC_EDGE_YUV_LESSER:
	default:
		if (renderer->hd_lite == FALSE)
			vert = vert_demosaic_edge_yuv;
		else
			vert = vert_demosaic_edge_yuv_lite;

		frag = frag_demosaic_edge_yuv_less;
		break;
	}

	/* build shaders and program */
	if (!gst_imx_egl_viv_trans_gles2_renderer_build_shader(&(renderer->vertex_shader), GL_VERTEX_SHADER, vert))
		return FALSE;
	if (!gst_imx_egl_viv_trans_gles2_renderer_build_shader(&(renderer->fragment_shader), GL_FRAGMENT_SHADER, frag))
		return FALSE;
	if (!gst_imx_egl_viv_trans_gles2_renderer_link_program(&(renderer->program), renderer->vertex_shader, renderer->fragment_shader))
		return FALSE;
	/* get uniform and attribute locations */
	renderer->source = glGetUniformLocation(renderer->program, "source");
	renderer->source_size = glGetUniformLocation(renderer->program, "sourceSize");
	renderer->red_coeff = glGetUniformLocation(renderer->program, "red_coeff");
	renderer->green_coeff = glGetUniformLocation(renderer->program, "green_coeff");
	renderer->blue_coeff = glGetUniformLocation(renderer->program, "blue_coeff");
	renderer->first_red = glGetUniformLocation(renderer->program, "firstRed");
	renderer->position_aloc = glGetAttribLocation(renderer->program, "a_position");
	renderer->texcoords_aloc = glGetAttribLocation(renderer->program, "a_texCoord");

	/* create texture */
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &(renderer->texture));
	glBindTexture(GL_TEXTURE_2D, renderer->texture);

	if (renderer->viv_ext == FALSE) {
		glTexImage2D(GL_TEXTURE_2D,
					0,
					GL_ALPHA,
					renderer->window_width,
					renderer->window_height,
					0,
					GL_ALPHA,
					GL_UNSIGNED_BYTE,
					NULL);
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glUniform4f(renderer->source_size,
			(float)renderer->window_width,
			(float)renderer->window_height,
			1.0 / (float)renderer->window_width,
			1.0 / (float)renderer->window_height);

	glUniform1i(renderer->source, 0);

	glUniform1f(renderer->red_coeff, renderer->red_coeff_value);
	glUniform1f(renderer->green_coeff, renderer->green_coeff_value);
	glUniform1f(renderer->blue_coeff, renderer->blue_coeff_value);

	switch(renderer->in_fmt) {
	case GST_EGL_TRANS_FORMAT_BGGR:
		glUniform2f(renderer->first_red, 1, 1);
		break;
	case GST_EGL_TRANS_FORMAT_GBRG:
		glUniform2f(renderer->first_red, 0, 1);
		break;
	case GST_EGL_TRANS_FORMAT_GRBG:
		glUniform2f(renderer->first_red, 1, 0);
		break;
	case GST_EGL_TRANS_FORMAT_RGGB:
	default:
		glUniform2f(renderer->first_red, 0, 0);
		break;
	}

	/* build vertex and index buffer objects */
	if (!gst_imx_egl_viv_trans_gles2_renderer_build_vertex_buffer(&(renderer->vertex_buffer)))
		return FALSE;

	/* enable vertex attrib array and set up pointers */
	glEnableVertexAttribArray(renderer->position_aloc);
	if (!CHECK_GL_ERROR("position vertex attrib", "glEnableVertexAttribArray"))
		return FALSE;
	glEnableVertexAttribArray(renderer->texcoords_aloc);
	if (!CHECK_GL_ERROR("texcoords vertex attrib", "glEnableVertexAttribArray"))
		return FALSE;

	glVertexAttribPointer(renderer->position_aloc,  vertex_position_num, GL_FLOAT, GL_FALSE, vertex_size, (GLvoid const*)((uintptr_t)vertex_position_offset));
	if (!CHECK_GL_ERROR("position vertex attrib", "glVertexAttribPointer"))
		return FALSE;
	glVertexAttribPointer(renderer->texcoords_aloc, vertex_texcoords_num, GL_FLOAT, GL_FALSE, vertex_size, (GLvoid const*)((uintptr_t)vertex_texcoords_offset));
	if (!CHECK_GL_ERROR("texcoords vertex attrib", "glVertexAttribPointer"))
		return FALSE;

	return TRUE;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_teardown_resources(GstImxEglVivTransGLES2Renderer *renderer)
{
	gboolean ret = TRUE;

	/* && ret instead of ret && to avoid early termination */
#if 0
	/* disable vertex attrib array and set up pointers */
	glDisableVertexAttribArray(renderer->position_aloc);
	ret = CHECK_GL_ERROR("position vertex attrib", "glDisableVertexAttribArray") && ret;
	glDisableVertexAttribArray(renderer->texcoords_aloc);
	ret = CHECK_GL_ERROR("texcoords vertex attrib", "glDisableVertexAttribArray") && ret;
#endif
	/* destroy vertex and index buffer objects */
	ret = gst_imx_egl_viv_trans_gles2_renderer_destroy_vertex_buffer(&(renderer->vertex_buffer)) && ret;

	/* destroy texture */
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &(renderer->texture));
#if 0
	/* destroy shaders and program */
	ret = gst_imx_egl_viv_trans_gles2_renderer_destroy_program(&(renderer->program), renderer->vertex_shader, renderer->fragment_shader) && ret;
	ret = gst_imx_egl_viv_trans_gles2_renderer_destroy_shader(&(renderer->vertex_shader), GL_VERTEX_SHADER) && ret;
	ret = gst_imx_egl_viv_trans_gles2_renderer_destroy_shader(&(renderer->fragment_shader), GL_FRAGMENT_SHADER) && ret;
#endif
	renderer->source = -1;
	renderer->source_size = -1;
	renderer->first_red = -1;
	renderer->position_aloc = -1;
	renderer->texcoords_aloc = -1;

	return ret;
}


static gboolean gst_imx_egl_viv_trans_gles2_renderer_fill_texture(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *buffer)
{
	GstMapInfo map_info;

	gst_buffer_map(buffer, &map_info, GST_MAP_READ);

	if (renderer->viv_ext != FALSE) {
		GstImxPhysMemMeta *phys_mem_meta;
		guint is_phys_buf;

		phys_mem_meta = GST_IMX_PHYS_MEM_META_GET(buffer);
		is_phys_buf = (phys_mem_meta != NULL) && (phys_mem_meta->phys_addr != 0);

		if (is_phys_buf) {
			GLvoid *virt_addr;
			GLuint phys_addr;

			phys_addr = (GLuint)(phys_mem_meta->phys_addr);
			virt_addr = map_info.data;

			glTexDirectVIVMap(
				GL_TEXTURE_2D,
				renderer->window_width,
				renderer->window_height,
				GL_ALPHA,
				(GLvoid **)(&virt_addr), &phys_addr
			);

			if (!CHECK_GL_ERROR("render", "glTexDirectVIVMap"))
				return FALSE;
		}
		else {
			size_t ysize;

			glTexDirectVIV(
				GL_TEXTURE_2D,
				renderer->window_width,
				renderer->window_height,
				GL_ALPHA,
				(GLvoid **) &(renderer->viv_planes)
			);

			if (!CHECK_GL_ERROR("render", "glTexDirectVIV"))
				return FALSE;

			ysize = renderer->window_width * renderer->window_height;
			memcpy(renderer->viv_planes[0], map_info.data, ysize);
		}

		glTexDirectInvalidateVIV(GL_TEXTURE_2D);
		if (!CHECK_GL_ERROR("render", "glTexDirectInvalidateVIV"))
			return FALSE;
	}
	else {
		glTexSubImage2D(GL_TEXTURE_2D,
					0,
					0,
					0,
					renderer->window_width,
					renderer->window_height,
					GL_ALPHA,
					GL_UNSIGNED_BYTE,
					map_info.data);
	}

	gst_buffer_unmap(buffer, &map_info);
	return TRUE;
}


static GstBuffer *gst_imx_egl_viv_trans_gles2_renderer_acquire_buffer(GstImxEglVivTransGLES2Renderer *renderer)
{
	GstBuffer *buffer = NULL;

	if (renderer->phys_alloc != FALSE) {
		GstFlowReturn flow_ret;

		flow_ret = gst_buffer_pool_acquire_buffer(renderer->pool, &buffer, NULL);
		if (flow_ret != GST_FLOW_OK) {
			GST_ERROR("could not allocate buffer from pool");
			buffer = NULL;
		}
	}
	else {
		GstMemory *mem;

		mem = gst_allocator_alloc(NULL, renderer->out_size, NULL);
		if (mem == NULL) {
			GST_ERROR("could not allocate memory");
			goto end;
		}

		buffer = gst_buffer_new();
		gst_buffer_append_memory(buffer, mem);
	}

end:
	return buffer;
}


gboolean gst_imx_egl_viv_trans_gles2_renderer_render_frame(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *src, GstBuffer *dest)
{
	const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
	gboolean ret;

	GST_LOG("rendering frame");

	glGetError(); /* clear out any existing error */

	if (!gst_imx_egl_viv_trans_gles2_renderer_fill_texture(renderer, src))
	{
		goto end;
	}

	if (renderer->offload != FALSE)
	{
		ret = gst_imx_egl_viv_trans_gles2_wait_fb_thread();
		if (ret == FALSE)
		{
			GST_ERROR("thread timeout");
			goto end;
		}
	}

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
	if (!CHECK_GL_ERROR("render", "glDrawArrays"))
	{
		goto end;
	}

	renderer->pts = GST_BUFFER_PTS(dest);
	renderer->dts = GST_BUFFER_DTS(dest);
	renderer->duration = GST_BUFFER_DURATION(dest);

	glFinish();

	if (renderer->offload == FALSE)
	{
		ret = gst_imx_egl_viv_trans_gles2_wait_fb_thread();
		if (ret == FALSE)
		{
			GST_ERROR("thread timeout");
			goto end;
		}

		gst_imx_egl_viv_trans_egl_platform_swap_buffers(renderer->egl_platform);
		glFinish();
	}

	gst_imx_egl_viv_trans_gles2_finish_render();

end:
	return TRUE;
}

static gboolean gst_imx_egl_viv_trans_gles2_wait_fb_thread(void)
{
	gboolean ret = TRUE;

	g_mutex_lock(&fb_mutex);
	while (fb_data && ret != FALSE) {
		gint64 end_time = g_get_monotonic_time() + 1000 * G_TIME_SPAN_MILLISECOND;
		ret = g_cond_wait_until(&fb_cond_prod, &fb_mutex, end_time);
	}

	if (fb_data) {
		/* timeout */
		ret = FALSE;
	}

	g_mutex_unlock(&fb_mutex);
	return ret;
}

static gboolean gst_imx_egl_viv_trans_gles2_wait_renderdata(void)
{
	gboolean ret = TRUE;

	g_mutex_lock(&fb_mutex);
	while (!thread_end && !fb_data && ret != FALSE) {
		gint64 end_time = g_get_monotonic_time() + 1000 * G_TIME_SPAN_MILLISECOND;
		ret = g_cond_wait_until(&fb_cond_cons, &fb_mutex, end_time);
	}

	if (thread_end || !fb_data) {
		ret = FALSE;
	}

	g_mutex_unlock(&fb_mutex);
	return ret;
}

static void gst_imx_egl_viv_trans_gles2_finish_render(void)
{
	g_mutex_lock(&fb_mutex);
	fb_data = TRUE;
	g_mutex_unlock(&fb_mutex);

	g_cond_signal(&fb_cond_cons);
}

static void gst_imx_egl_viv_trans_gles2_renderdata_rcv_done(void)
{
	g_mutex_lock(&fb_mutex);
	fb_data = FALSE;
	g_mutex_unlock(&fb_mutex);

	g_cond_signal(&fb_cond_prod);
}

static void gst_imx_egl_viv_trans_gles2_copy_fb(GstImxEglVivTransGLES2Renderer *renderer)
{
	gboolean ret;
	GstMapInfo map_info;
	GstBuffer *dest;

	ret = gst_imx_egl_viv_trans_gles2_wait_renderdata();
	if (ret != FALSE) {
		GstFlowReturn flow_ret;
		void *ptr = renderer->fb_map;

		if (renderer->offload != FALSE) {
			gst_imx_egl_viv_trans_egl_platform_swap_buffers(renderer->egl_platform);
		}

		dest = gst_imx_egl_viv_trans_gles2_renderer_acquire_buffer(renderer);
		if (dest == NULL) {
			GST_ERROR("could not allocate frame");
			goto end;
		}

		dest = gst_buffer_make_writable(dest);

		GST_BUFFER_PTS(dest) = renderer->pts;
		GST_BUFFER_DTS(dest) = renderer->dts;
		GST_BUFFER_DURATION(dest) = renderer->duration;

		if (renderer->offload != FALSE) {
			glFinish();
			gst_imx_egl_viv_trans_gles2_renderdata_rcv_done();
		}

		if (renderer->useipu == FALSE) {
			gst_buffer_map(dest, &map_info, GST_MAP_WRITE);
			memcpy(map_info.data, ptr, renderer->out_size);
			gst_buffer_unmap(dest, &map_info);
		}
		else {
			GstImxPhysMemMeta *phys_mem_meta;
			guint is_phys_buf;
			unsigned long p_dstaddr;
			void *v_dstaddr;

			phys_mem_meta = GST_IMX_PHYS_MEM_META_GET(dest);
			is_phys_buf = (phys_mem_meta != NULL) && (phys_mem_meta->phys_addr != 0);
			if (is_phys_buf) {
				p_dstaddr = (unsigned long)phys_mem_meta->phys_addr;
				v_dstaddr = NULL;
			}
			else {
				gst_buffer_map(dest, &map_info, GST_MAP_WRITE);
				p_dstaddr = renderer->ipu_paddr;
				v_dstaddr = map_info.data;
			}

			if (renderer->hdiv > 0) {
				gst_imx_bayer_ipu_yuv_conv_div(renderer->out_width,
									renderer->window_height,
									renderer->fb_paddr,
									p_dstaddr,
									renderer->ipu_vaddr,
									v_dstaddr,
									renderer->ipu_in_fmt,
									renderer->ipu_out_fmt,
									renderer->ipu_out_size,
									renderer->hdiv,
									renderer->vdiv);
			}
			else {
				gst_imx_bayer_ipu_yuv_conv(renderer->out_width,
									renderer->window_height,
									renderer->fb_paddr,
									p_dstaddr,
									renderer->ipu_vaddr,
									v_dstaddr,
									renderer->ipu_in_fmt,
									renderer->ipu_out_fmt,
									renderer->ipu_out_size);
			}

			if (v_dstaddr != NULL) {
				gst_buffer_unmap(dest, &map_info);
			}
		}

		if (renderer->offload == FALSE) {
			gst_imx_egl_viv_trans_gles2_renderdata_rcv_done();
		}

		flow_ret = gst_pad_push(renderer->push_pad, dest);
		if (flow_ret != GST_FLOW_OK) {
			GST_INFO("gst_pad_push error(%d)", (int)flow_ret);
		}
	}

end:
	return;
}


GstImxEglVivTransGLES2Renderer* gst_imx_egl_viv_trans_gles2_renderer_create(char const *native_display_name)
{
	GstImxEglVivTransGLES2Renderer *renderer;

	init_debug_category();

	renderer = g_slice_alloc(sizeof(GstImxEglVivTransGLES2Renderer));

	renderer->init = FALSE;
	renderer->window_width = 0;
	renderer->window_height = 0;
	renderer->packed_window_width = 0;
	renderer->demosaic = 0;
	renderer->hd_lite = FALSE;
	renderer->egl_platform = NULL;

	renderer->in_fmt = 0;
	renderer->out_fmt = 0;

	renderer->vertex_shader = 0;
	renderer->fragment_shader = 0;
	renderer->program = 0;
	renderer->vertex_buffer = 0;
	renderer->texture = 0;

	renderer->source = -1;
	renderer->source_size = -1;
	renderer->first_red = -1;
	renderer->position_aloc = -1;
	renderer->texcoords_aloc = -1;

	renderer->red_coeff_value = 0.0;
	renderer->green_coeff_value = 0.0;
	renderer->blue_coeff_value = 0.0;

	renderer->thread = NULL;
	renderer->push_pad = NULL;
	renderer->allocator = NULL;
	renderer->pool = NULL;
	renderer->phys_alloc = FALSE;
	renderer->offload = FALSE;
	renderer->pts = GST_CLOCK_TIME_NONE;
	renderer->dts = GST_CLOCK_TIME_NONE;
	renderer->duration = GST_CLOCK_TIME_NONE;
	renderer->out_size = 0;

	renderer->viv_ext = TRUE;
	renderer->viv_planes[0] = NULL;

	renderer->useipu = FALSE;
	renderer->ipu_in_fmt = 0;
	renderer->ipu_out_fmt = 0;
	renderer->ipu_out_size = 0;
	renderer->hdiv = -1;
	renderer->vdiv = -1;
	renderer->ipu_vaddr = NULL;
	renderer->fb_map = NULL;
	renderer->fb_paddr = 0;
	renderer->map_len = 0;

	snprintf(renderer->fb_name,
			sizeof(renderer->fb_name),
			"/dev/fb%s",
			native_display_name);
	strncpy(renderer->display_name,
			native_display_name,
			sizeof(renderer->display_name) - 1);

	fb_data = FALSE;
	thread_end = FALSE;

	return renderer;
}

void gst_imx_egl_viv_trans_gles2_renderer_destroy(GstImxEglVivTransGLES2Renderer *renderer)
{
	if (renderer == NULL)
		return;

	if (renderer->thread != NULL)
	{
		g_mutex_lock(&fb_mutex);
		fb_data = TRUE;
		thread_end = TRUE;
		g_mutex_unlock(&fb_mutex);
		g_cond_signal(&fb_cond_cons);

		g_thread_join(renderer->thread);
	}

	GST_INFO("stopping renderer");
	gst_imx_egl_viv_trans_gles2_renderer_stop(renderer);

	if (renderer->egl_platform != NULL)
	{
		GST_INFO("destroying EGL platform");
		gst_imx_egl_viv_trans_egl_platform_destroy(renderer->egl_platform);
	}

	if (renderer->allocator != NULL)
	{
		gst_object_unref(renderer->allocator);
		renderer->allocator = NULL;
	}

	if (renderer->push_pad != NULL)
	{
		gst_object_unref(renderer->push_pad);
		renderer->push_pad = NULL;
	}

	renderer->init = FALSE;

	g_slice_free1(sizeof(GstImxEglVivTransGLES2Renderer), renderer);
}

