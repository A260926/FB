/*
 * Texture Manager
 */

#include "zgl.h"

static GLTexture* find_texture(GLint h) {
	GLTexture* t;
	GLContext* c = gl_get_context();
	t = c->shared_state.texture_hash_table[h & TEXTURE_HASH_TABLE_MASK];
	while (t != NULL) {
		if (t->handle == h)
			return t;
		t = t->next;
	}
	return NULL;
}

GLboolean glAreTexturesResident(GLsizei n, const GLuint* textures, GLboolean* residences) {
#define RETVAL GL_FALSE
	GLboolean retval = GL_TRUE;
	GLint i;
#include "error_check_no_context.h"

	for (i = 0; i < n; i++)
		if (find_texture(textures[i])) {
			residences[i] = GL_TRUE;
		} else {
			residences[i] = GL_FALSE;
			retval = GL_FALSE;
		}
	return retval;
}
GLboolean glIsTexture(GLuint texture) {
	GLContext* c = gl_get_context();
#define RETVAL GL_FALSE
#include "error_check.h"
	if (find_texture(texture))
		return GL_TRUE;
	return GL_FALSE;
}

void* glGetTexturePixmap(GLint text, GLint level, GLint* xsize, GLint* ysize) {
	GLTexture* tex;
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (!(text >= 0 && level < MAX_TEXTURE_LEVELS))
#define ERROR_FLAG GL_INVALID_ENUM
#define RETVAL NULL
#include "error_check.h"
#else
	/*assert(text >= 0 && level < MAX_TEXTURE_LEVELS);*/
#endif
		tex = find_texture(text);
	if (!tex)
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_ENUM
#define RETVAL NULL
#include "error_check.h"
#else
		return NULL;
#endif
		*xsize = tex->images[level].xsize;
	*ysize = tex->images[level].ysize;
	if (tex->images[level].is_depth)
		return tex->images[level].depth;
	return tex->images[level].pixmap;
}

static void free_texture_image_storage(GLImage* im) {
	if (im->depth) {
		gl_free(im->depth);
		im->depth = NULL;
	}
	im->is_depth = 0;
}

static void free_texture(GLContext* c, GLint h) {
	GLTexture *t, **ht;
	GLint level;

	t = find_texture(h);
	if (t->prev == NULL) {
		ht = &c->shared_state.texture_hash_table[t->handle & TEXTURE_HASH_TABLE_MASK];
		*ht = t->next;
	} else {
		t->prev->next = t->next;
	}
	if (t->next != NULL)
		t->next->prev = t->prev;

	for (level = 0; level < MAX_TEXTURE_LEVELS; level++)
		free_texture_image_storage(&t->images[level]);
	gl_free(t);
}

GLTexture* alloc_texture(GLint h) {
	GLContext* c = gl_get_context();
	GLTexture *t, **ht;
#define RETVAL NULL
#include "error_check.h"
	t = gl_zalloc(sizeof(GLTexture));
	if (!t)
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#define RETVAL NULL
#include "error_check.h"
#else
		gl_fatal_error("GL_OUT_OF_MEMORY");
#endif

		ht = &c->shared_state.texture_hash_table[h & TEXTURE_HASH_TABLE_MASK];

	t->next = *ht;
	t->prev = NULL;
	if (t->next != NULL)
		t->next->prev = t;
	*ht = t;

	t->handle = h;
	/* depth texture defaults: no comparison, LEQUAL, NEAREST */
	t->compare_mode = GL_NONE;
	t->compare_func = GL_LEQUAL;
	t->min_filter = GL_NEAREST;
	t->mag_filter = GL_NEAREST;

	return t;
}

void glInitTextures() {
	/* textures */
	GLContext* c = gl_get_context();
	c->texture_2d_enabled = 0;
	c->current_texture = find_texture(0);
}

void glGenTextures(GLint n, GLuint* textures) {
	GLContext* c = gl_get_context();
	GLint max, i;
	GLTexture* t;
#include "error_check.h"
	max = 0;
	for (i = 0; i < TEXTURE_HASH_TABLE_SIZE; i++) {
		t = c->shared_state.texture_hash_table[i];
		while (t != NULL) {
			if (t->handle > max)
				max = t->handle;
			t = t->next;
		}
	}
	for (i = 0; i < n; i++) {
		textures[i] = max + i + 1; /* MARK: How texture handles are created.*/
	}
}

void glDeleteTextures(GLint n, const GLuint* textures) {
	GLint i;
	GLTexture* t;
	GLContext* c = gl_get_context();
#include "error_check.h"
	for (i = 0; i < n; i++) {
		t = find_texture(textures[i]);
		if (t != NULL && t != 0) {
			if (t == c->current_texture) {
				glBindTexture(GL_TEXTURE_2D, 0);
#include "error_check.h"
			}
			free_texture(c, textures[i]);
		}
	}
}

void glopBindTexture(GLParam* p) {
	GLint target = p[1].i;
	GLint texture = p[2].i;
	GLTexture* t;
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (!(target == GL_TEXTURE_2D && target > 0))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"
#else
	
#endif
		t = find_texture(texture);
	if (t == NULL) {
		t = alloc_texture(texture);
#include "error_check.h"
	}
	if (t == NULL) { 
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
		gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
	}
	c->current_texture = t;
}


void glCopyTexImage2D(GLenum target,		 
					  GLint level,			 
					  GLenum internalformat, 
					  GLint x,				 
					  GLint y,				 
					  GLsizei width,		 
					  GLsizei height, GLint border) {
	GLParam p[9];
#include "error_check_no_context.h"

	p[0].op = OP_CopyTexImage2D;
	p[1].i = target;
	p[2].i = level;
	p[3].i = internalformat;
	p[4].i = x;
	p[5].i = y;
	p[6].i = width;
	p[7].i = height;
	p[8].i = border;
	gl_add_op(p);
}
void glopCopyTexImage2D(GLParam* p) {
	GLImage* im;
	PIXEL* data;
	GLint i, j;
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint internalformat = p[3].i;
	GLint x = p[4].i;
	GLint y = p[5].i;
	GLsizei w = p[6].i;
	GLsizei h = p[7].i;
	GLint border = p[8].i;
	GLContext* c = gl_get_context();

	if (c->readbuffer != GL_FRONT || c->current_texture == NULL || target != GL_TEXTURE_2D || border != 0 ||
		w != TGL_FEATURE_TEXTURE_DIM || /*TODO Implement image interp*/
		h != TGL_FEATURE_TEXTURE_DIM) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_OPERATION
#include "error_check.h"
#else
		return;
#endif
	}
	im = &c->current_texture->images[level];
	im->xsize = TGL_FEATURE_TEXTURE_DIM;
	im->ysize = TGL_FEATURE_TEXTURE_DIM;

	if (internalformat == GL_DEPTH_COMPONENT) {
		GLfloat* depth;
		GLint xsize = c->zb->xsize;
		GLint ysize = c->zb->ysize;
		if (!im->depth)
			im->depth = gl_malloc(sizeof(GLfloat) * TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM);
		if (!im->depth) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		im->is_depth = 1;
		depth = im->depth;
		/* independent snapshot of the current ZBuffer: 16 bit camera Z
		   (0 = far after clear) maps to normalized 1 - z/65535.
		   x/y are lower-left window coordinates; texture row 0 holds
		   the bottom region row, so framebuffer memory row (top origin)
		   is ysize-1-(y+j). */
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++) {
				GLint sx = x + i;
				GLint sy = ysize - 1 - (y + j);
				GLushort zz = c->zb->zbuf[sy * xsize + sx];
				depth[j * w + i] = 1.0f - (GLfloat)zz / 65535.0f;
			}
		return;
	}

	/* RGB copy replaces any previous depth storage */
	free_texture_image_storage(im);
	data = c->current_texture->images[level].pixmap;
	y -= h;

	/* TODO implement the scaling and stuff that the GL spec says it should have.*/
#if TGL_FEATURE_MULTITHREADED_COPY_TEXIMAGE_2D == 1
#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			data[i + j * w] = c->zb->pbuf[((i + x) % (c->zb->xsize)) + ((j + y) % (c->zb->ysize)) * (c->zb->xsize)];
		}
#else
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			data[i + j * w] = c->zb->pbuf[((i + x) % (c->zb->xsize)) + ((j + y) % (c->zb->ysize)) * (c->zb->xsize)];
		}
#endif
}

void glopTexImage1D(GLParam* p) {
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint components = p[3].i;
	GLint width = p[4].i;
	/* GLint height = p[5].i;*/
	GLint height = 1;
	GLint border = p[5].i;
	GLint format = p[6].i;
	GLint type = p[7].i;
	void* pixels = p[8].p;
	GLImage* im;
	GLubyte* pixels1;
	GLint do_free=0;
	GLContext* c = gl_get_context();
	{
#if TGL_FEATURE_ERROR_CHECK == 1
		if (!(c->current_texture != NULL && target == GL_TEXTURE_1D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"

#else
		if (!(c->current_texture != NULL && target == GL_TEXTURE_1D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
			gl_fatal_error("glTexImage2D: combination of parameters not handled!!");
#endif
	}
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		pixels1 = gl_malloc(TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM * 3); /* GUARDED*/
		if (pixels1 == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		/* no GLinterpolation is done here to respect the original image aliasing ! */
		
		gl_resizeImageNoInterpolate(pixels1, TGL_FEATURE_TEXTURE_DIM, TGL_FEATURE_TEXTURE_DIM, pixels, width, height);
		do_free = 1;
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM; 
	} else {
		pixels1 = pixels;
	}

	im = &c->current_texture->images[level];
	im->xsize = width;
	im->ysize = height;
#if TGL_FEATURE_RENDER_BITS == 32
	gl_convertRGB_to_8A8R8G8B(im->pixmap, pixels1, width, height);
#elif TGL_FEATURE_RENDER_BITS == 16
	gl_convertRGB_to_5R6G5B(im->pixmap, pixels1, width, height);
#else
#error bad TGL_FEATURE_RENDER_BITS
#endif
	if (do_free)
		gl_free(pixels1);
}
void glopTexImage2D(GLParam* p) {
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint components = p[3].i;
	GLint width = p[4].i;
	GLint height = p[5].i;
	GLint border = p[6].i;
	GLint format = p[7].i;
	GLint type = p[8].i;
	void* pixels = p[9].p;
	GLImage* im;
	GLubyte* pixels1;
	GLint do_free=0;
	GLContext* c = gl_get_context();

	if (components == GL_DEPTH_COMPONENT) {
		GLint count;
		if (!(c->current_texture != NULL && target == GL_TEXTURE_2D && level == 0 && border == 0 &&
			  format == GL_DEPTH_COMPONENT && type == GL_FLOAT &&
			  width == TGL_FEATURE_TEXTURE_DIM && height == TGL_FEATURE_TEXTURE_DIM)) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"
#else
			gl_fatal_error("glTexImage2D: combination of parameters not handled!!");
#endif
		}
		im = &c->current_texture->images[level];
		count = TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM;
		if (!im->depth)
			im->depth = gl_malloc(sizeof(GLfloat) * count);
		if (!im->depth) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		/* copy the caller's data: later clears, draws or overwriting the
		   source array must not change the texture */
		memcpy(im->depth, pixels, sizeof(GLfloat) * count);
		im->is_depth = 1;
		im->xsize = width;
		im->ysize = height;
		return;
	}
	{
#if TGL_FEATURE_ERROR_CHECK == 1
		if (!(c->current_texture != NULL && target == GL_TEXTURE_2D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"

#else
		if (!(c->current_texture != NULL && target == GL_TEXTURE_2D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
			gl_fatal_error("glTexImage2D: combination of parameters not handled!!");
#endif
	}
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		pixels1 = gl_malloc(TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM * 3); /* GUARDED*/
		if (pixels1 == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		/* no GLinterpolation is done here to respect the original image aliasing ! */
		
		gl_resizeImageNoInterpolate(pixels1, TGL_FEATURE_TEXTURE_DIM, TGL_FEATURE_TEXTURE_DIM, pixels, width, height);
		do_free = 1;
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM;
	} else {
		pixels1 = pixels;
	}

	im = &c->current_texture->images[level];
	/* an RGB redefinition releases any previous depth storage */
	free_texture_image_storage(im);
	im->xsize = width;
	im->ysize = height;
#if TGL_FEATURE_RENDER_BITS == 32
	gl_convertRGB_to_8A8R8G8B(im->pixmap, pixels1, width, height);
#elif TGL_FEATURE_RENDER_BITS == 16
	gl_convertRGB_to_5R6G5B(im->pixmap, pixels1, width, height);
#else
#error Bad TGL_FEATURE_RENDER_BITS
#endif
	if (do_free)
		gl_free(pixels1);
}

void glopTexParameter(GLParam* p) {
	GLint target = p[1].i;
	GLint pname = p[2].i;
	GLint param = p[3].i;
	GLContext* c = gl_get_context();
	GLTexture* t;
	if (target != GL_TEXTURE_2D || c->current_texture == NULL)
		return;
	t = c->current_texture;
	switch (pname) {
	case GL_TEXTURE_COMPARE_MODE:
		if (param == GL_NONE || param == GL_COMPARE_R_TO_TEXTURE)
			t->compare_mode = param;
		break;
	case GL_TEXTURE_COMPARE_FUNC:
		if (param == GL_LEQUAL || param == GL_LESS)
			t->compare_func = param;
		break;
	case GL_TEXTURE_MIN_FILTER:
		if (param == GL_NEAREST || param == GL_LINEAR)
			t->min_filter = param;
		break;
	case GL_TEXTURE_MAG_FILTER:
		if (param == GL_NEAREST || param == GL_LINEAR)
			t->mag_filter = param;
		break;
	default:
		break;
	}
}

/* TODO: not all tests are done */
/*
void glopTexEnv(GLContext* c, GLParam* p) {
	GLint target = p[1].i;
	GLint pname = p[2].i;
	GLint param = p[3].i;

	if (target != GL_TEXTURE_ENV) {

	error:
#if TGL_FEATURE_ERROR_CHECK == 1

#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"
#else
		gl_fatal_error("glTexParameter: unsupported option");
#endif

	}

	if (pname != GL_TEXTURE_ENV_MODE)
		goto error;

	if (param != GL_DECAL)
		goto error;
}
*/
/* TODO: not all tests are done */
/*
void glopTexParameter(GLContext* c, GLParam* p) {
	GLint target = p[1].i;
	GLint pname = p[2].i;
	GLint param = p[3].i;

	if (target != GL_TEXTURE_2D &&
		target != GL_TEXTURE_1D) {
	error:
		tgl_warning("glTexParameter: unsupported option");
		return;
	}

	switch (pname) {
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (param != GL_REPEAT)
			goto error;
		break;
	}
}
*/

/*
void glopPixelStore(GLContext* c, GLParam* p) {
	GLint pname = p[1].i;
	GLint param = p[2].i;

	if (pname != GL_UNPACK_ALIGNMENT || param != 1) {
		gl_fatal_error("glPixelStore: unsupported option");
	}
}
*/
