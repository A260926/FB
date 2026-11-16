/*
 * Framebuffer objects
 *
 * A non zero framebuffer owns a private depth buffer while its color
 * buffer aliases the level 0 image of an attached RGB texture.  Drawing
 * into such a framebuffer therefore writes directly into the texture's
 * pixels and is immediately visible through the normal texturing path.
 */

#include "zgl.h"

/* Depth storage uses the same dimensions as a texture image. */
#define FB_DIM TGL_FEATURE_TEXTURE_DIM

static GLboolean image_is_defined(const GLTexture* t) {
	return t != NULL && t->images[0].xsize == FB_DIM && t->images[0].ysize == FB_DIM;
}

GLTexture* gl_find_texture(GLint h) {
	GLContext* c = gl_get_context();
	GLTexture* t;
	t = c->shared_state.texture_hash_table[h & TEXTURE_HASH_TABLE_MASK];
	while (t != NULL) {
		if (t->handle == h)
			return t;
		t = t->next;
	}
	return NULL;
}

GLFramebuffer* gl_fb_lookup(GLuint name) {
	GLContext* c = gl_get_context();
	if (name == 0 || name > MAX_FRAMEBUFFERS)
		return NULL;
	return c->framebuffers[name - 1];
}

GLint gl_fb_is_complete(GLFramebuffer* fb) {
	if (fb == NULL)
		return 1; /* name 0 is the default framebuffer, always complete */
	return image_is_defined(fb->color_texture);
}

/* Copy the render state that is stored on ZBuffer between targets. */
void gl_fb_sync_raster_state(ZBuffer* dst, const ZBuffer* src) {
	dst->enable_blend = src->enable_blend;
	dst->sfactor = src->sfactor;
	dst->dfactor = src->dfactor;
	dst->blendeq = src->blendeq;
	dst->depth_test = src->depth_test;
	dst->depth_write = src->depth_write;
	dst->pointsize = src->pointsize;
#if TGL_FEATURE_POLYGON_STIPPLE == 1
	dst->dostipple = src->dostipple;
	memcpy(dst->stipplepattern, src->stipplepattern, TGL_POLYGON_STIPPLE_BYTES);
#endif
	dst->current_texture = NULL;
}

/* Propagate a raster state change to every existing target. */
void gl_fb_state_changed(void) {
	GLContext* c = gl_get_context();
	ZBuffer* src = c->zb;
	GLint i;
	if (c->framebuffers == NULL)
		return;
	if (c->default_zb != NULL && src != c->default_zb)
		gl_fb_sync_raster_state(c->default_zb, src);
	for (i = 0; i < MAX_FRAMEBUFFERS; i++) {
		GLFramebuffer* fb = c->framebuffers[i];
		if (fb != NULL && fb->zb != NULL && fb->zb != src)
			gl_fb_sync_raster_state(fb->zb, src);
	}
}

/*
 * Allocate the private depth buffer of a framebuffer.  The color pointer
 * stays NULL until a texture is attached; it is never owned here.
 */
static ZBuffer* fb_open_zb(void) {
	ZBuffer* zb;
	GLint size;
	zb = gl_zalloc(sizeof(ZBuffer));
	if (zb == NULL)
		return NULL;
	zb->xsize = FB_DIM & ~3;
	zb->ysize = FB_DIM;
	zb->linesize = FB_DIM * PSZB;
	size = zb->xsize * zb->ysize * (GLint)sizeof(GLushort);
	zb->zbuf = gl_malloc(size);
	if (zb->zbuf == NULL) {
		gl_free(zb);
		return NULL;
	}
	memset(zb->zbuf, 0, size); /* the original far value is zero */
	zb->pbuf = NULL;
	zb->frame_buffer_allocated = 0;
	gl_fb_sync_raster_state(zb, gl_get_context()->zb);
	return zb;
}

static void fb_clear_depth(GLFramebuffer* fb) {
	GLint size = fb->zb->xsize * fb->zb->ysize * (GLint)sizeof(GLushort);
	memset(fb->zb->zbuf, 0, size);
}

static GLFramebuffer* fb_create(GLuint name) {
	GLContext* c = gl_get_context();
	GLFramebuffer* fb = gl_zalloc(sizeof(GLFramebuffer));
	if (fb == NULL)
		return NULL;
	fb->handle = name;
	fb->color_texture = NULL;
	fb->zb = fb_open_zb();
	if (fb->zb == NULL) {
		gl_free(fb);
		return NULL;
	}
	c->framebuffers[name - 1] = fb;
	return fb;
}

static void fb_destroy(GLFramebuffer* fb) {
	GLContext* c = gl_get_context();
	if (fb == NULL)
		return;
	c->framebuffers[fb->handle - 1] = NULL;
	gl_free(fb->zb->zbuf);
	gl_free(fb->zb);
	gl_free(fb);
}

void glInitFramebuffers(void) {
	GLContext* c = gl_get_context();
	c->default_zb = c->zb;
	c->framebuffer_binding = 0;
	c->framebuffers = gl_zalloc(sizeof(GLFramebuffer*) * MAX_FRAMEBUFFERS);
	if (c->framebuffers == NULL)
		gl_fatal_error("TINYGL_CANNOT_INIT_OOM");
}

void glEndFramebuffers(void) {
	GLContext* c = gl_get_context();
	GLint i;
	if (c->framebuffers == NULL)
		return;
	for (i = 0; i < MAX_FRAMEBUFFERS; i++) {
		if (c->framebuffers[i] != NULL)
			fb_destroy(c->framebuffers[i]);
	}
	gl_free(c->framebuffers);
	c->framebuffers = NULL;
	c->framebuffer_binding = 0;
	c->zb = c->default_zb;
}

void glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
	GLContext* c = gl_get_context();
	GLuint names[MAX_FRAMEBUFFERS];
	GLsizei left;
	GLuint i;
	GLint slot;
#if TGL_FEATURE_ERROR_CHECK == 1
	if (c->in_begin) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
	if (n < 0) {
		c->error_flag = GL_INVALID_VALUE;
		return;
	}
#endif
	left = n;
	for (slot = 1; slot <= MAX_FRAMEBUFFERS && left > 0; slot++) {
		if (c->framebuffers[slot - 1] == NULL)
			names[--left] = (GLuint)slot;
	}
	if (left > 0) {
#if TGL_FEATURE_ERROR_CHECK == 1
		c->error_flag = GL_INVALID_VALUE;
#endif
		for (i = 0; i < (GLuint)n; i++)
			framebuffers[i] = 0;
		return;
	}
	for (i = 0; i < (GLuint)n; i++) {
		if (fb_create(names[i]) == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
			c->error_flag = GL_OUT_OF_MEMORY;
#endif
			framebuffers[i] = 0;
			return;
		}
		framebuffers[i] = names[i];
	}
}

void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
	GLContext* c = gl_get_context();
	GLsizei i;
#if TGL_FEATURE_ERROR_CHECK == 1
	if (c->in_begin) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
	if (n < 0) {
		c->error_flag = GL_INVALID_VALUE;
		return;
	}
#endif
	for (i = 0; i < n; i++) {
		GLFramebuffer* fb = gl_fb_lookup(framebuffers[i]);
		if (fb == NULL)
			continue; /* name 0 and unused names are silently ignored */
		if (c->framebuffer_binding == fb->handle) {
			c->framebuffer_binding = 0;
			c->zb = c->default_zb;
		}
		fb_destroy(fb);
	}
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
	GLContext* c = gl_get_context();
	GLFramebuffer* fb;
#if TGL_FEATURE_ERROR_CHECK == 1
	if (target != GL_FRAMEBUFFER) {
		c->error_flag = GL_INVALID_ENUM;
		return;
	}
	if (c->in_begin) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
	if (framebuffer != 0 && gl_fb_lookup(framebuffer) == NULL) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
#endif
	if (framebuffer == 0) {
		c->framebuffer_binding = 0;
		c->zb = c->default_zb;
		return;
	}
	fb = gl_fb_lookup(framebuffer);
	c->framebuffer_binding = framebuffer;
	c->zb = fb->zb;
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level) {
	GLContext* c = gl_get_context();
	GLFramebuffer* fb;
	GLTexture* t;
#if TGL_FEATURE_ERROR_CHECK == 1
	if (target != GL_FRAMEBUFFER) {
		c->error_flag = GL_INVALID_ENUM;
		return;
	}
	if (attachment != GL_COLOR_ATTACHMENT0 || textarget != GL_TEXTURE_2D) {
		c->error_flag = GL_INVALID_ENUM;
		return;
	}
	if (level != 0) {
		c->error_flag = GL_INVALID_VALUE;
		return;
	}
	if (c->in_begin || c->framebuffer_binding == 0) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
	if (texture != 0 && gl_find_texture(texture) == NULL) {
		c->error_flag = GL_INVALID_OPERATION;
		return;
	}
#endif
	fb = gl_fb_lookup(c->framebuffer_binding);
	if (fb == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
		c->error_flag = GL_INVALID_OPERATION;
#endif
		return;
	}
	if (texture == 0) {
		/* detach: the target keeps its depth storage but has no color image */
		if (fb->color_texture != NULL) {
			fb->color_texture = NULL;
			fb->zb->pbuf = NULL;
		}
		return;
	}
	t = gl_find_texture(texture);
	if (t == fb->color_texture) {
		/* re-attaching the same image keeps the depth contents */
		return;
	}
	fb->color_texture = t;
	fb->zb->pbuf = t->images[0].pixmap;
	if (t != fb->depth_texture) {
		/* a different image: the depth starts from the original far value */
		fb->depth_texture = t;
		fb_clear_depth(fb);
	}
}

GLenum glCheckFramebufferStatus(GLenum target) {
	GLContext* c = gl_get_context();
	GLFramebuffer* fb;
#if TGL_FEATURE_ERROR_CHECK == 1
	if (target != GL_FRAMEBUFFER) {
		c->error_flag = GL_INVALID_ENUM;
		return 0;
	}
	if (c->in_begin) {
		c->error_flag = GL_INVALID_OPERATION;
		return 0;
	}
#endif
	if (c->framebuffer_binding == 0)
		return GL_FRAMEBUFFER_COMPLETE;
	fb = gl_fb_lookup(c->framebuffer_binding);
	if (fb != NULL && gl_fb_is_complete(fb))
		return GL_FRAMEBUFFER_COMPLETE;
	return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}

/* Remove a texture from every framebuffer that references it. */
void gl_fb_detach_texture(GLTexture* t) {
	GLContext* c = gl_get_context();
	GLint i;
	for (i = 0; i < MAX_FRAMEBUFFERS; i++) {
		GLFramebuffer* fb = c->framebuffers[i];
		if (fb != NULL && fb->color_texture == t) {
			fb->color_texture = NULL;
			fb->depth_texture = NULL; /* a future attachment must reset depth */
			fb->zb->pbuf = NULL;
		}
	}
}

/* Re-bind the storage of a redefined texture image and reset its depths. */
void gl_fb_refresh_texture(GLTexture* t) {
	GLContext* c = gl_get_context();
	GLint i;
	for (i = 0; i < MAX_FRAMEBUFFERS; i++) {
		GLFramebuffer* fb = c->framebuffers[i];
		if (fb != NULL && fb->color_texture == t) {
			fb->zb->pbuf = t->images[0].pixmap;
			fb->depth_texture = t;
			fb_clear_depth(fb);
		}
	}
}

/*
 * Validation run just before a primitive submission (glopBegin).
 * Returns the error to raise, or GL_NO_ERROR.  An incomplete target and
 * a texture feedback loop both suppress the whole submission.
 */
GLenum gl_fb_begin_check(void) {
	GLContext* c = gl_get_context();
	GLFramebuffer* fb;
	if (c->framebuffer_binding == 0)
		return GL_NO_ERROR;
	fb = gl_fb_lookup(c->framebuffer_binding);
	if (fb == NULL || !gl_fb_is_complete(fb))
		return GL_INVALID_FRAMEBUFFER_OPERATION;
	if (c->texture_2d_enabled && c->current_texture != NULL &&
	    c->current_texture == fb->color_texture)
		return GL_INVALID_OPERATION;
	return GL_NO_ERROR;
}
