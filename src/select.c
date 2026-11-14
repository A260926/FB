#include "zgl.h"

GLint glRenderMode(GLint mode) {
	GLContext* c = gl_get_context();
	GLint result = 0;
#if TGL_FEATURE_ALT_RENDERMODES == 1
	switch (c->render_mode) {
	case GL_RENDER:
		break;
	case GL_SELECT:
		if (c->select_overflow) {
			result = -c->select_hits;
		} else {
			result = c->select_hits;
		}
		c->select_overflow = 0;
		c->select_ptr = c->select_buffer;
		c->name_stack_size = 0;
		break;
	case GL_FEEDBACK:
		if (c->feedback_overflow)
			/* negative even when not a single complete record fit */
			result = (c->feedback_hits > 0) ? -c->feedback_hits : -1;
		else
			result = c->feedback_hits;
		c->feedback_overflow = 0;
		c->feedback_hits = 0;
		c->feedback_ptr = c->feedback_buffer;
		break;
	default:
		gl_fatal_error("GLContext's Rendermode was somehow erroneously set.");
	}
	switch (mode) {
	case GL_RENDER:
		c->render_mode = GL_RENDER;
		break;
	case GL_SELECT:

#if TGL_FEATURE_ERROR_CHECK == 1
		if (c->select_buffer == NULL)
#define ERROR_FLAG GL_INVALID_OPERATION
#define RETVAL 0
#include "error_check.h"
#else
		if (c->select_buffer == NULL)
			return 0;
#endif
			c->render_mode = GL_SELECT;
		c->select_ptr = c->select_buffer;
		c->select_hits = 0;
		c->select_overflow = 0;
		c->select_hit = NULL;
		break;
	case GL_FEEDBACK:
#if TGL_FEATURE_ERROR_CHECK == 1
		if (c->feedback_buffer == NULL)
#define ERROR_FLAG GL_INVALID_OPERATION
#define RETVAL 0
#include "error_check.h"
#else
		if (c->feedback_buffer == NULL)
			return 0;
#endif
			c->render_mode = GL_FEEDBACK;
		c->feedback_hits = 0;
		c->feedback_ptr = c->feedback_buffer;
		c->feedback_overflow = 0;
		break;
	default:
#if TGL_FEATURE_ERROR_CHECK == 1
#define RETVAL 0
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"

#else
		return 0;
#endif
	}
	return result;
#else
	
	return 0;
#endif
}

void glSelectBuffer(GLint size, GLuint* buf) {

#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (c->render_mode == GL_SELECT)
#define ERROR_FLAG GL_INVALID_OPERATION
#include "error_check.h"
#else
	if (c->render_mode == GL_SELECT)
		return;
#endif

		c->select_buffer = buf;
	c->select_size = size;
#else
	return;
#endif
}

void glFeedbackBuffer(GLint size, GLenum type, GLfloat* buf) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (c->render_mode == GL_FEEDBACK || !(type == GL_2D || type == GL_3D || type == GL_3D_COLOR || type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE))
#define ERROR_FLAG GL_INVALID_OPERATION
#include "error_check.h"
#else
	if (c->render_mode == GL_FEEDBACK || !(type == GL_2D || type == GL_3D || type == GL_3D_COLOR || type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE))
		return;
#endif
		c->feedback_buffer = buf;
	c->feedback_size = size;
	c->feedback_type = type;
#else
	return;
#endif
}

#if TGL_FEATURE_ALT_RENDERMODES == 1
/* number of floats written per feedback vertex for each feedback type */
static GLuint gl_feedback_vertex_size(GLenum type) {
	switch (type) {
	case GL_2D:
		return 2; /* x y */
	case GL_3D:
		return 3; /* x y z */
	case GL_3D_COLOR:
		return 3 + 4; /* x y z, rgba */
	case GL_3D_COLOR_TEXTURE:
		return 3 + 4 + 4; /* x y z, rgba, stq */
	case GL_4D_COLOR_TEXTURE:
		return 4 + 4 + 4; /* x y z w, rgba, stq */
	default:
		return 0;
	}
}

static GLuint gl_feedback_vertex_pos(GLenum type) {
	switch (type) {
	case GL_2D:
		return 2;
	case GL_3D:
	case GL_3D_COLOR:
	case GL_3D_COLOR_TEXTURE:
		return 3;
	case GL_4D_COLOR_TEXTURE:
		return 4;
	default:
		return 0;
	}
}

static void gl_write_feedback_vertex(GLContext* c, GLVertex* v) {
	GLenum type = c->feedback_type;
	GLuint pos = gl_feedback_vertex_pos(type);

	/* window coordinates are floating point feedback values, never the
	   integer ZBufferPoint rasterization coordinates */
	if (pos >= 2) {
		*(c->feedback_ptr++) = v->fb.X;
		*(c->feedback_ptr++) = v->fb.Y;
	}
	if (pos >= 3)
		*(c->feedback_ptr++) = v->fb.Z;
	if (pos >= 4)
		*(c->feedback_ptr++) = v->fb.W;

	if (type == GL_3D_COLOR || type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE) {
		*(c->feedback_ptr++) = v->color.X;
		*(c->feedback_ptr++) = v->color.Y;
		*(c->feedback_ptr++) = v->color.Z;
		*(c->feedback_ptr++) = v->color.W;
	}
	if (type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE) {
		*(c->feedback_ptr++) = v->tex_coord.X;
		*(c->feedback_ptr++) = v->tex_coord.Y;
		*(c->feedback_ptr++) = v->tex_coord.Z;
		*(c->feedback_ptr++) = v->tex_coord.W;
	}
}
#endif

void gl_add_feedback(GLfloat token, GLVertex* v1, GLVertex* v2, GLVertex* v3, GLfloat passthrough_token_value) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	GLuint vertex_size, record_size, vertex_count, i;
	GLVertex* verts[3];

	/* only the feedback render mode emits records; this also keeps
	   PassThrough safe when no feedback buffer is bound */
	if (c->render_mode != GL_FEEDBACK)
		return;

	/* once the buffer overflowed the write pointer is locked for the
	   remainder of this feedback pass */
	if (c->feedback_overflow)
		return;

	vertex_size = gl_feedback_vertex_size(c->feedback_type);

	if (token == GL_PASS_THROUGH_TOKEN) {
		vertex_count = 0;
		record_size = 2; /* token + passthrough value */
	} else if (token == GL_POINT_TOKEN || token == GL_BITMAP_TOKEN || token == GL_DRAW_PIXEL_TOKEN || token == GL_COPY_PIXEL_TOKEN) {
		vertex_count = 1;
		record_size = 1 + vertex_size;
	} else if (token == GL_LINE_TOKEN || token == GL_LINE_RESET_TOKEN) {
		vertex_count = 2;
		record_size = 1 + 2 * vertex_size;
	} else if (token == GL_POLYGON_TOKEN) {
		vertex_count = 3;
		record_size = 1 + 1 + 3 * vertex_size; /* token + vertex count + vertices */
	} else {
		return;
	}

	/* a record is either written completely or not at all */
	if ((GLuint)c->feedback_hits + record_size > c->feedback_size) {
		c->feedback_overflow = 1;
		return;
	}

	/* tokens and polygon vertex counts are numeric float values, not bit casts */
	*(c->feedback_ptr++) = token;

	if (token == GL_PASS_THROUGH_TOKEN) {
		*(c->feedback_ptr++) = passthrough_token_value;
		c->feedback_hits += record_size;
		return;
	}

	if (token == GL_POLYGON_TOKEN)
		*(c->feedback_ptr++) = (GLfloat)vertex_count;

	verts[0] = v1;
	verts[1] = v2;
	verts[2] = v3;
	for (i = 0; i < vertex_count; i++)
		gl_write_feedback_vertex(c, verts[i]);

	c->feedback_hits += record_size;
#endif
	return;
}

void glopPassThrough(GLParam* p) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	gl_add_feedback(GL_PASS_THROUGH_TOKEN, NULL, NULL, NULL, p[1].f);
#endif
}

void glPassThrough(GLfloat token) {

#include "error_check_no_context.h"
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLParam p[2];
	p[0].op = OP_PassThrough;
	p[1].f = token;
	/* routed through the op chain so display lists record it; the call is
	   only emitted into the feedback buffer when the list is executed while
	   GL_FEEDBACK is the current render mode */
	gl_add_op(p);
#else
	return;
#endif
}
void glopInitNames(GLParam* p) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	if (c->render_mode == GL_SELECT) {
		c->name_stack_size = 0;
		c->select_hit = NULL;
	}
#endif
}

void glopPushName(GLParam* p) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	if (c->render_mode == GL_SELECT) {
		
		c->name_stack[c->name_stack_size++] = p[1].i;
		c->select_hit = NULL;
	}
#endif
}

void glopPopName(GLParam* p) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	if (c->render_mode == GL_SELECT) {
		
		c->name_stack_size--;
		c->select_hit = NULL;
	}
#endif
}

void glopLoadName(GLParam* p) {
#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	if (c->render_mode == GL_SELECT) {
		
		c->name_stack[c->name_stack_size - 1] = p[1].i;
		c->select_hit = NULL;
	}
#endif
}

void gl_add_select(GLuint zmin, GLuint zmax) {

#if TGL_FEATURE_ALT_RENDERMODES == 1
	GLContext* c = gl_get_context();
	GLuint* ptr;
	GLint n, i;
	if (!c->select_overflow) {
		if (c->select_hit == NULL) {
			n = c->name_stack_size;
			if ((c->select_ptr - c->select_buffer + 3 + n) > c->select_size) { 
				c->select_overflow = 1;
			} else {
				ptr = c->select_ptr;
				c->select_hit = ptr;
				*ptr++ = c->name_stack_size;
				*ptr++ = zmin;
				*ptr++ = zmax;
				for (i = 0; i < n; i++)
					*ptr++ = c->name_stack[i];
				c->select_ptr = ptr;
				c->select_hits++;
			}
		} else {
			if (zmin < c->select_hit[1])
				c->select_hit[1] = zmin;
			if (zmax > c->select_hit[2])
				c->select_hit[2] = zmax;
		}
	}
#else
	return;
#endif
}
