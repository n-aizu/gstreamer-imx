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

#include "gl_headers.h"
#include "gles2_renderer.h"
#include "egl_platform.h"
#include "ipu.h"
#include "../common/phys_mem_meta.h"


GST_DEBUG_CATEGORY_STATIC(imx_gles2transfer_debug);
#define GST_CAT_DEFAULT imx_gles2transfer_debug


struct _GstImxEglVivTransGLES2Renderer
{
	gint in_fmt;
	guint window_width, window_height;
	GstImxEglVivTransEGLPlatform *egl_platform;

	GLuint vertex_shader, fragment_shader, program;
	GLuint vertex_buffer;
	GLuint texture;

	GLint source, source_size, first_red;
	GLint red_coeff, green_coeff, blue_coeff;
	GLint position_aloc, texcoords_aloc;

	float red_coeff_value;
	float green_coeff_value;
	float blue_coeff_value;
	unsigned char v_value;

	gboolean ext_mapped;
	gboolean viv_ext;
	GLvoid* viv_planes[3];

	char fb_name[16];
	char display_name[8];
};

struct _GstImxEglVivTransFb
{
	gint out_fmt;
	guint window_width, window_height;

	unsigned long ipu_paddr;
	void *ipu_vaddr;

	size_t map_len;
	void* fb_map;
	unsigned long fb_paddr;

	gint fb_num;
};

static GMutex fb_mutex;
static GCond fb_cond_prod, fb_cond_cons;
static volatile gboolean fb_data;
static volatile gboolean fb_first;

static gboolean gst_imx_egl_viv_trans_setfb(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_mapfb(GstImxEglVivTransFb *fbdata);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_check_gl_error(char const *category, char const *label, int line);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_shader(GLuint *shader, GLenum shader_type, char const *code);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_shader(GLuint *shader, GLenum shader_type);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_link_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_program(GLuint *program, GLuint vertex_shader, GLuint fragment_shader);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_build_vertex_buffer(GLuint *vertex_buffer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_destroy_vertex_buffer(GLuint *vertex_buffer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_search_extension(GLubyte const *extensions);

static gboolean gst_imx_egl_viv_trans_gles2_renderer_setup_resources(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_teardown_resources(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_fill_texture(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *buffer);
static gboolean gst_imx_egl_viv_trans_gles2_renderer_commit_texture(GstImxEglVivTransGLES2Renderer *renderer);
static gboolean gst_imx_egl_viv_trans_gles2_wait_fbdata(void);
static void gst_imx_egl_viv_trans_gles2_finish_fbcopy(void);

#define CHECK_GL_ERROR(str1, str2) \
	gst_imx_egl_viv_trans_gles2_renderer_check_gl_error(str1, str2, __LINE__)

/* http://graphics.cs.williams.edu/papers/BayerJGT09/ */
static char const *vert_demosaic =
	"varying vec4 kC = vec4( 4.0,  6.0,  5.0,  5.0) / 8.0;\n"
	"varying vec4 kA = vec4(-1.0, -1.5,  0.5, -1.0) / 8.0;\n"
	"varying vec4 kB = vec4( 2.0,  0.0,  0.0,  4.0) / 8.0;\n"
	"varying vec4 kD = vec4( 0.0,  2.0, -1.0, -1.0) / 8.0;\n"

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

static char const *frag_demosaic =
	/** Monochrome RGBA or GL_LUMINANCE Bayer encoded texture.*/
	"uniform sampler2D  source;\n"
	"uniform float red_coeff;\n"
	"uniform float green_coeff;\n"
	"uniform float blue_coeff;\n"
	"varying vec4 center;\n"
	"varying vec4 yCoord;\n"
	"varying vec4 xCoord;\n"

	"varying vec4 kC;\n"
	"varying vec4 kA;\n"
	"varying vec4 kB;\n"
	"varying vec4 kD;\n"

	"void main(void) {\n"
	"    #define fetch(x, y) texture2D(source, vec2(x, y)).r\n"
	"    float C = texture2D(source, center.xy).r; // ( 0, 0)\n"

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
	"    Dvec.xy += Dvec.zw;\n"
	"    Dvec.x  += Dvec.y;\n"

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

	"    vec3 tmp_rgb = (alternate.y == 0.0) ?\n"
	"        ((alternate.x == 0.0) ?\n"
	"            vec3(C, PATTERN.xy) :\n"
	"            vec3(PATTERN.z, C, PATTERN.w)) :\n"
	"        ((alternate.x == 0.0) ?\n"
	"            vec3(PATTERN.w, C, PATTERN.z) :\n"
	"            vec3(PATTERN.yx, C));\n"
	"    vec3 coeff = vec3(red_coeff, green_coeff, blue_coeff);\n"
	"    gl_FragColor.rgb = tmp_rgb * coeff;\n"
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



static void init_debug_category(void)
{
	static gboolean initialized = FALSE;
	if (!initialized)
	{
		GST_DEBUG_CATEGORY_INIT(imx_gles2transfer_debug, "imxgles2transfer", 0, "imxeglvivtrans OpenGL ES 2 videotranser renderer");
		initialized = TRUE;
	}
}


static gboolean gst_imx_egl_viv_trans_setfb(GstImxEglVivTransGLES2Renderer *renderer)
{
	int ret, fd;
	int width, height;
	struct fb_var_screeninfo var;
	gboolean bret = FALSE;

	fd = open(renderer->fb_name, O_RDONLY);
	if (fd < 0) {
		GST_ERROR("open error(%s)", strerror(errno));
		goto end;
	}

		width = renderer->window_width;
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
		}
	else {
		bret = TRUE;
	}

	close(fd);
end:
	return bret;
}


static gboolean gst_imx_egl_viv_trans_mapfb(GstImxEglVivTransFb *fbdata)
{
	int ret, fd;
	void *ptr;
	size_t len;
	struct fb_fix_screeninfo finfo;
	gboolean bret = FALSE;
	char fb_name[16];

	snprintf(fb_name, sizeof(fb_name), "/dev/fb%d", fbdata->fb_num);
	fd = open(fb_name, O_RDONLY);
	if (fd < 0) {
		GST_ERROR("open error(%s)", strerror(errno));
		goto end;
	}

	ret = ioctl(fd, FBIOGET_FSCREENINFO, &finfo);
	if (ret < 0) {
		GST_ERROR("ioctl error(%s)", strerror(errno));
		goto close_end;
	}

	fbdata->fb_paddr = finfo.smem_start;

	len = fbdata->window_width * fbdata->window_height * 4; /* BGRA */
	ptr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ptr == MAP_FAILED) {
		GST_ERROR("mmap error(%s)", strerror(errno));
		goto close_end;
	}

	fbdata->fb_map = ptr;
	fbdata->map_len = len;
	bret = TRUE;

close_end:
	close(fd);
end:
	return bret;
}




gboolean gst_imx_egl_viv_trans_gles2_renderer_setup(GstImxEglVivTransGLES2Renderer *renderer, int width, int height, int in_fmt, float red_coeff, float green_coeff, float blue_coeff, unsigned int fbset, unsigned int extbuf, unsigned int chrom)
{
	GLubyte const *extensions;

	renderer->window_width = width;
	renderer->window_height = height;
	renderer->in_fmt = in_fmt;
	renderer->red_coeff_value = red_coeff;
	renderer->green_coeff_value = green_coeff;
	renderer->blue_coeff_value = blue_coeff;
	renderer->ext_mapped = (extbuf ? TRUE : FALSE);
	renderer->v_value = (unsigned char)chrom;

	if (fbset)
	{
		if (!gst_imx_egl_viv_trans_setfb(renderer))
		{
			GST_ERROR("framebuffer ioctl error");
			return FALSE;
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
		renderer->window_width, renderer->window_height
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
		GST_ERROR("setting up resources failed - stopping thread");

		return FALSE;
	}

	glBindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer);
	glBindTexture(GL_TEXTURE_2D, renderer->texture);

	GST_INFO("starting GLES2 renderer");
	return TRUE;
}


gboolean gst_imx_egl_viv_trans_gles2_fb_setup(GstImxEglVivTransFb *fbdata, int width, int height, int out_fmt)
{
	fbdata->window_width = width;
	fbdata->window_height = height;
	fbdata->out_fmt = out_fmt;

	if (!gst_imx_egl_viv_trans_mapfb(fbdata))
	{
		GST_ERROR("framebuffer mmap error");
		return FALSE;
	}

	if (out_fmt == GST_EGL_TRANS_FORMAT_I420)
	{
		if (!gst_imx_bayer_ipu_yuv_init(fbdata->window_width,
										fbdata->window_height,
										&fbdata->ipu_paddr,
										&fbdata->ipu_vaddr))
		{
			GST_ERROR("ipu init error");
			return FALSE;
		}
	}

	return TRUE;
}


void gst_imx_egl_viv_trans_gles2_renderer_stop(GstImxEglVivTransGLES2Renderer *renderer)
{
	if (!gst_imx_egl_viv_trans_gles2_renderer_teardown_resources(renderer))
		GST_ERROR("tearing down resources failed");

	if (!gst_imx_egl_viv_trans_egl_platform_shutdown_window(renderer->egl_platform))
		GST_ERROR("could not close window");
}


void gst_imx_egl_viv_trans_gles2_fb_deinit(GstImxEglVivTransFb *fbdata)
{
	if (fbdata->out_fmt == GST_EGL_TRANS_FORMAT_I420)
	{
		if (!gst_imx_bayer_ipu_yuv_end(fbdata->window_width,
										fbdata->window_height,
										fbdata->ipu_paddr,
										fbdata->ipu_vaddr))
			GST_ERROR("IPU end error");
	}

	munmap(fbdata->fb_map, fbdata->map_len);
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
	/* must be called with lock */

	/* build shaders and program */
	if (!gst_imx_egl_viv_trans_gles2_renderer_build_shader(&(renderer->vertex_shader), GL_VERTEX_SHADER, vert_demosaic))
		return FALSE;
	if (!gst_imx_egl_viv_trans_gles2_renderer_build_shader(&(renderer->fragment_shader), GL_FRAGMENT_SHADER, frag_demosaic))
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
					GL_LUMINANCE,
					renderer->window_width,
					renderer->window_height,
					0,
					GL_LUMINANCE,
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
	/* must be called with lock */

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
		size_t ysize, uvsize;

		phys_mem_meta = GST_IMX_PHYS_MEM_META_GET(buffer);
		is_phys_buf = (phys_mem_meta != NULL) && (phys_mem_meta->phys_addr != 0);

		ysize = renderer->window_width * renderer->window_height;
		uvsize = ysize / 4;

		if (renderer->ext_mapped != FALSE && is_phys_buf) {
			GLvoid *virt_addr;
			GLuint phys_addr;
			char *ptr;
			size_t uoffset;

			phys_addr = (GLuint)(phys_mem_meta->phys_addr);
			virt_addr = map_info.data;

			/* https://community.freescale.com/message/343739#343739 */
			ptr = (char *)map_info.data;
			uoffset = ysize + uvsize;
			memset((ptr + uoffset), renderer->v_value, uvsize);

			/* glTexDirectVIVMap is async API */
			glTexDirectVIVMap(
				GL_TEXTURE_2D,
				renderer->window_width,
				renderer->window_height,
				GL_VIV_I420,
				(GLvoid **)(&virt_addr), &phys_addr
			);

			if (!CHECK_GL_ERROR("render", "glTexDirectVIVMap"))
				return FALSE;
		}
		else {
			glTexDirectVIV(
				GL_TEXTURE_2D,
				renderer->window_width,
				renderer->window_height,
				GL_VIV_I420,
				(GLvoid **) &(renderer->viv_planes)
			);

			if (!CHECK_GL_ERROR("render", "glTexDirectVIV"))
				return FALSE;

			memcpy(renderer->viv_planes[0], map_info.data, ysize);
			memset(renderer->viv_planes[2], renderer->v_value, uvsize);
		}
	}
	else {
		glTexSubImage2D(GL_TEXTURE_2D,
					0,
					0,
					0,
					renderer->window_width,
					renderer->window_height,
					GL_LUMINANCE,
					GL_UNSIGNED_BYTE,
					map_info.data);
	}

	gst_buffer_unmap(buffer, &map_info);
	return TRUE;
}

static gboolean gst_imx_egl_viv_trans_gles2_renderer_commit_texture(GstImxEglVivTransGLES2Renderer *renderer)
{
	gboolean ret = FALSE;

	if (renderer->viv_ext != FALSE) {
		glTexDirectInvalidateVIV(GL_TEXTURE_2D);
		if (!CHECK_GL_ERROR("render", "glTexDirectInvalidateVIV")) {
			goto end;
		}
	}

	ret = TRUE;
end:
	return ret;
}

gboolean gst_imx_egl_viv_trans_gles2_renderer_render_frame_onethread(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *src, G_GNUC_UNUSED GstBuffer *dest)
{
	GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
	GstImxEglVivTransEGLPlatform *platform = renderer->egl_platform;
	gboolean ret = FALSE;

	GST_LOG("rendering frame");

	glGetError(); /* clear out any existing error */

	if (!gst_imx_egl_viv_trans_gles2_renderer_fill_texture(renderer, src))
	{
		goto end;
	}

	glClear(GL_COLOR_BUFFER_BIT);
	if (!CHECK_GL_ERROR("render", "glClear"))
	{
		goto end;
	}

	if (!gst_imx_egl_viv_trans_gles2_renderer_commit_texture(renderer))
	{
		goto end;
	}

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
	if (!CHECK_GL_ERROR("render", "glDrawArrays"))
	{
		goto end;
	}

	gst_imx_egl_viv_trans_egl_platform_swap_buffers(platform);
	glFinish();

	ret = TRUE;
end:
	return ret;
}

gboolean gst_imx_egl_viv_trans_gles2_renderer_render_frame(GstImxEglVivTransGLES2Renderer *renderer, GstBuffer *src, G_GNUC_UNUSED GstBuffer *dest)
{
	GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
	GstImxEglVivTransEGLPlatform *platform = renderer->egl_platform;
	gboolean ret = FALSE;

	GST_LOG("rendering frame");

	glGetError(); /* clear out any existing error */

	/* GPU processing time for demosaic is about two times of */
	/* IPU processing time for colorspace conversion.         */
	/* So wait 1 frame before start of IPU.                   */
	if (fb_first) {
		fb_first = FALSE;
	}
	else {
		gboolean swap_done = FALSE;

		/* optimization */
		if (!fb_data) {
			gst_imx_egl_viv_trans_egl_platform_swap_buffers(platform);
			swap_done = TRUE;
		}

		/* optimization - call glFinish regardless of calling swap_buffers */
		/* (somehow this way is more efficient)                            */
		glFinish();

		g_mutex_lock(&fb_mutex);
		if (!swap_done) {
			while(fb_data) {
				gint64 end_time = g_get_monotonic_time() + 1000 * G_TIME_SPAN_MILLISECOND;
				gboolean wait_ret = g_cond_wait_until(&fb_cond_prod, &fb_mutex, end_time);
				if (wait_ret == FALSE) {
					/* timeout: overwrite framebuffer */
					break;
				}
			}

			gst_imx_egl_viv_trans_egl_platform_swap_buffers(platform);
			glFinish();
		}

		fb_data = TRUE;
		g_mutex_unlock(&fb_mutex);

		g_cond_signal(&fb_cond_cons);
	}

	if (!gst_imx_egl_viv_trans_gles2_renderer_fill_texture(renderer, src))
	{
		goto end;
	}

	glClear(GL_COLOR_BUFFER_BIT);
	if (!CHECK_GL_ERROR("render", "glClear"))
	{
		goto end;
	}

	if (!gst_imx_egl_viv_trans_gles2_renderer_commit_texture(renderer))
	{
		goto end;
	}

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
	if (!CHECK_GL_ERROR("render", "glDrawArrays"))
	{
		goto end;
	}

	ret = TRUE;
end:
	return ret;
}


static gboolean gst_imx_egl_viv_trans_gles2_wait_fbdata(void)
{
	gboolean ret = TRUE;

	g_mutex_lock(&fb_mutex);
	while(!fb_data && ret != FALSE) {
		gint64 end_time = g_get_monotonic_time() + 1000 * G_TIME_SPAN_MILLISECOND;
		ret = g_cond_wait_until(&fb_cond_cons, &fb_mutex, end_time);
	}

	if (!fb_data) {
		/* timeout */
		g_mutex_unlock(&fb_mutex);
		ret = FALSE;
	}

	return ret;
}

static void gst_imx_egl_viv_trans_gles2_finish_fbcopy(void)
{
	fb_data = FALSE;
	g_mutex_unlock(&fb_mutex);
	g_cond_signal(&fb_cond_prod);
}

gboolean gst_imx_egl_viv_trans_gles2_copy_fb_onethread(GstImxEglVivTransFb *fbdata, G_GNUC_UNUSED GstBuffer *src, GstBuffer *dest)
{
	gboolean ret = FALSE;
	GstMapInfo map_info;
	void *ptr;
	size_t len;

	gst_buffer_map(dest, &map_info, GST_MAP_WRITE);
	ptr = fbdata->fb_map;
	len = fbdata->map_len;

	if (fbdata->out_fmt == GST_EGL_TRANS_FORMAT_BGRA) {
		memcpy(map_info.data, ptr, len);
	}
	else {
		/* GST_EGL_TRANS_FORMAT_I420 */

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
			p_dstaddr = fbdata->ipu_paddr;
			v_dstaddr = map_info.data;
		}

		if (fbdata->window_width > 1024 ||
			fbdata->window_height > 1024) {
			int vdiv, hdiv;

			if (fbdata->window_height >= 1080) {
				vdiv = 3;
			}
			else if (fbdata->window_height > 1024) {
				vdiv = 2;
			}
			else {
				vdiv = 1;
			}

			if (fbdata->window_width > 1024) {
				hdiv = 2;
			}
			else {
				hdiv = 1;
			}

			gst_imx_bayer_ipu_yuv_conv_div(fbdata->window_width,
								fbdata->window_height,
								fbdata->fb_paddr,
								p_dstaddr,
								fbdata->ipu_vaddr,
								v_dstaddr,
								hdiv,
								vdiv);
		}
		else {
			gst_imx_bayer_ipu_yuv_conv(fbdata->window_width,
								fbdata->window_height,
								fbdata->fb_paddr,
								p_dstaddr,
								fbdata->ipu_vaddr,
								v_dstaddr);
		}
	}

	gst_buffer_unmap(dest, &map_info);
	ret = TRUE;
	return ret;
}

gboolean gst_imx_egl_viv_trans_gles2_copy_fb(GstImxEglVivTransFb *fbdata, G_GNUC_UNUSED GstBuffer *src, GstBuffer *dest)
{
	gboolean ret = FALSE, wait_ret;
	GstMapInfo map_info;
	void *ptr;
	size_t len;

	gst_buffer_map(dest, &map_info, GST_MAP_WRITE);
	ptr = fbdata->fb_map;
	len = fbdata->map_len;

	if (fbdata->out_fmt == GST_EGL_TRANS_FORMAT_BGRA) {
		wait_ret = gst_imx_egl_viv_trans_gles2_wait_fbdata();
		if (wait_ret != FALSE) {
			memcpy(map_info.data, ptr, len);
			gst_imx_egl_viv_trans_gles2_finish_fbcopy();
		}
	}
	else {
		/* GST_EGL_TRANS_FORMAT_I420 */

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
			p_dstaddr = fbdata->ipu_paddr;
			v_dstaddr = map_info.data;
		}

		if (fbdata->window_width > 1024 ||
			fbdata->window_height > 1024) {
			int vdiv, hdiv;

			if (fbdata->window_height >= 1080) {
				vdiv = 3;
			}
			else if (fbdata->window_height > 1024) {
				vdiv = 2;
			}
			else {
				vdiv = 1;
			}

			if (fbdata->window_width > 1024) {
				hdiv = 2;
			}
			else {
				hdiv = 1;
			}

			wait_ret = gst_imx_egl_viv_trans_gles2_wait_fbdata();
			if (wait_ret != FALSE) {
				gst_imx_bayer_ipu_yuv_conv_div(fbdata->window_width,
									fbdata->window_height,
									fbdata->fb_paddr,
									p_dstaddr,
									fbdata->ipu_vaddr,
									v_dstaddr,
									hdiv,
									vdiv);
				gst_imx_egl_viv_trans_gles2_finish_fbcopy();
			}
		}
		else {
			wait_ret = gst_imx_egl_viv_trans_gles2_wait_fbdata();
			if (wait_ret != FALSE) {
				gst_imx_bayer_ipu_yuv_conv(fbdata->window_width,
									fbdata->window_height,
									fbdata->fb_paddr,
									p_dstaddr,
									fbdata->ipu_vaddr,
									v_dstaddr);
				gst_imx_egl_viv_trans_gles2_finish_fbcopy();
			}
		}
	}

	gst_buffer_unmap(dest, &map_info);
	ret = TRUE;
	return ret;
}


GstImxEglVivTransGLES2Renderer* gst_imx_egl_viv_trans_gles2_renderer_create(char const *native_display_name)
{
	GstImxEglVivTransGLES2Renderer *renderer;

	init_debug_category();

	renderer = g_slice_alloc(sizeof(GstImxEglVivTransGLES2Renderer));

	renderer->window_width = 0;
	renderer->window_height = 0;
#if 0
	renderer->egl_platform = gst_imx_egl_viv_trans_egl_platform_create(
		native_display_name
	);
	if (renderer->egl_platform == NULL)
	{
		g_slice_free1(sizeof(GstImxEglVivTransGLES2Renderer), renderer);
		return NULL;
	}
#else
	renderer->egl_platform = NULL;
#endif

	renderer->in_fmt = 0;

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
	renderer->v_value = 0;

	renderer->ext_mapped = FALSE;
	renderer->viv_ext = TRUE;
	renderer->viv_planes[0] = NULL;

	snprintf(renderer->fb_name,
			sizeof(renderer->fb_name),
			"/dev/fb%s",
			native_display_name);
	strncpy(renderer->display_name,
			native_display_name,
			sizeof(renderer->display_name) - 1);

	fb_data = FALSE;
	fb_first = TRUE;

	return renderer;
}


GstImxEglVivTransFb* gst_imx_egl_viv_trans_gles2_fbdata_create(int fb_num)
{
	GstImxEglVivTransFb *fbdata;

	init_debug_category();

	fbdata = g_slice_alloc(sizeof(GstImxEglVivTransFb));

	fbdata->out_fmt = 0;
	fbdata->window_width = 0;
	fbdata->window_height = 0;

	fbdata->map_len = 0;
	fbdata->fb_map = NULL;

	fbdata->fb_num = fb_num;

	return fbdata;
}


void gst_imx_egl_viv_trans_gles2_renderer_destroy(GstImxEglVivTransGLES2Renderer *renderer)
{
	if (renderer == NULL)
		return;

	g_mutex_lock(&fb_mutex);
	fb_data = TRUE;
	g_mutex_unlock(&fb_mutex);
	g_cond_signal(&fb_cond_cons);

	GST_INFO("stopping renderer");
	gst_imx_egl_viv_trans_gles2_renderer_stop(renderer);

	if (renderer->egl_platform != NULL)
	{
		GST_INFO("destroying EGL platform");
		gst_imx_egl_viv_trans_egl_platform_destroy(renderer->egl_platform);
	}

	g_slice_free1(sizeof(GstImxEglVivTransGLES2Renderer), renderer);
}

