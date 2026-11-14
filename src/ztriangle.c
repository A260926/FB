#include "../include/zbuffer.h"
#include "msghandling.h"
#include <stdlib.h>
#include <math.h>




/* TODO: Switch from scanline rasterizer to easily parallelized cross product rasterizer.*/
static GLfloat edgeFunction(GLfloat ax, GLfloat ay, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

#if TGL_FEATURE_RENDER_BITS == 32
#elif TGL_FEATURE_RENDER_BITS == 16
#else
#error "WRONG MODE!!!"
#endif

#if TGL_FEATURE_POLYGON_STIPPLE == 1

#define TGL_STIPPLEVARS                                                                                                                                        \
	GLubyte* zbstipplepattern = zb->stipplepattern;                                                                                                            \
	GLubyte zbdostipple = zb->dostipple;
#define THE_X ((GLint)(pp - pp1))
#define XSTIP(_a) ((THE_X + _a) & TGL_POLYGON_STIPPLE_MASK_X)
#define YSTIP (the_y & TGL_POLYGON_STIPPLE_MASK_Y)
/* NOTES                                                           Divide by 8 to get the byte        Get the actual bit*/
#define STIPBIT(_a) (zbstipplepattern[(XSTIP(_a) | (YSTIP << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >> 3] & (1 << (XSTIP(_a) & 7)))
#define STIPTEST(_a) &&(!(zbdostipple && !STIPBIT(_a)))

#else

#define TGL_STIPPLEVARS /* a comment */
#define STIPTEST(_a)	/* a comment*/

#endif

#if TGL_FEATURE_NO_DRAW_COLOR == 1
#define NODRAWTEST(c) &&((c & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#else
#define NODRAWTEST(c) /* a comment */
#endif

#define ZCMP(z, zpix, _a, c) (((!zbdt) || (z >= zpix)) STIPTEST(_a) NODRAWTEST(c))
#define ZCMPSIMP(z, zpix, _a, crabapple) (((!zbdt) || (z >= zpix)) STIPTEST(_a))

void ZB_fillTriangleFlat(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
	GLuint color;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ

#define INTERP_Z


#define DRAW_INIT()                                                                                                                                            \
	{ color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); }

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, color)) {                                                                                                             \
				TGL_BLEND_FUNC(color, (pp[_a])) /*pp[_a] = color;*/                                                                                            \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

void ZB_fillTriangleFlatNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#define INTERP_Z

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = color;                                                                                                                                \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

/*
 * Smooth filled triangle.
 * The code below is very tricky :)
 */

void ZB_fillTriangleSmooth(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}


#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif

#include "ztriangle.h"
} 

void ZB_fillTriangleSmoothNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}

#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			/*c = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                               \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#endif

#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif
/* End of 16 bit mode stuff*/
#include "ztriangle.h"
} 

/*


			TEXTURE MAPPED TRIANGLES
               Section_Header




*/
void ZB_setTexture(ZBuffer* zb, PIXEL* texture) { zb->current_texture = texture; }


#if 1

#define DRAW_LINE_TRI_TEXTURED()                                                                                                                               \
	{                                                                                                                                                          \
		register GLushort* pz;                                                                                                                                 \
		register PIXEL* pp;                                                                                                                                    \
		register GLuint s, t, z;                                                                                                                               \
		register GLint n;                                                                                                                                      \
		OR1OG1OB1DECL                                                                                                                                          \
		GLfloat sz, tz, fzl, zinv;                                                                                                                             \
		n = (x2 >> 16) - x1;                                                                                                                                   \
		fzl = (GLfloat)z1;                                                                                                                                     \
		zinv = 1.0 / fzl;                                                                                                                                      \
		pp = (PIXEL*)((GLbyte*)pp1 + x1 * PSZB);                                                                                                               \
		pz = pz1 + x1;                                                                                                                                         \
		z = z1;                                                                                                                                                \
		sz = sz1;                                                                                                                                              \
		tz = tz1;                                                                                                                                              \
		while (n >= (NB_INTERP - 1)) {                                                                                                                         \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			fzl += fndzdx;                                                                                                                                     \
			zinv = 1.0 / fzl;                                                                                                                                  \
			PUT_PIXEL(0); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(1); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(2); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(3); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(4); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(5); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(6); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(7); /*the_x-=7;*/                                                                                                                        \
			pz += NB_INTERP;                                                                                                                                   \
			pp += NB_INTERP; /*the_x+=NB_INTERP * PSZB;*/                                                                                                      \
			n -= NB_INTERP;                                                                                                                                    \
			sz += ndszdx;                                                                                                                                      \
			tz += ndtzdx;                                                                                                                                      \
		}                                                                                                                                                      \
		{                                                                                                                                                      \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			while (n >= 0) {                                                                                                                                   \
				PUT_PIXEL(0);                                                                                                                                  \
				pz += 1;                                                                                                                                       \
				/*pp = (PIXEL*)((GLbyte*)pp + PSZB);*/                                                                                                         \
				pp++;                                                                                                                                          \
				n -= 1;                                                                                                                                        \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
	} 

void ZB_fillTriangleMappingPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB


#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));*/                                                                       \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, (TEXTURE_SAMPLE(texture, s, t))), (pp[_a]));                                                        \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));                                                                                      \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }

#include "ztriangle.h"
}

void ZB_fillTriangleMappingPerspectiveNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;
	
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));                                                                           \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, c);                                                                                                       \
				/*TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));*/                                                                                  \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }
#include "ztriangle.h"
}

#endif 

/*

		   DEPTH (SHADOW) TEXTURE MAPPED TRIANGLES

*/

void ZB_setDepthTexture(ZBuffer* zb, const GLfloat* depth, GLint dim, GLint compare_mode, GLint compare_func, GLint linear) {
	zb->depth_texture = depth;
	zb->depth_texture_dim = dim;
	zb->depth_compare_mode = compare_mode;
	zb->depth_compare_func = compare_func;
	zb->depth_linear = linear;
}

void ZB_clearDepthTexture(ZBuffer* zb) {
	zb->depth_texture = NULL;
	zb->depth_texture_dim = 0;
	zb->depth_compare_mode = GL_NONE;
	zb->depth_compare_func = GL_LEQUAL;
	zb->depth_linear = 0;
}

static GLint tgl_depth_clampi(GLint i, GLint dim) {
	if (i < 0)
		return 0;
	if (i > dim - 1)
		return dim - 1;
	return i;
}

static GLfloat tgl_depth_fetch(const GLfloat* depth, GLint dim, GLint ix, GLint iy) {
	return depth[tgl_depth_clampi(iy, dim) * dim + tgl_depth_clampi(ix, dim)];
}

/* clamp-to-edge depth lookup; returns either the stored depth gray
   (GL_NONE) or the 0/1 comparison result, filtered as configured */
static GLfloat tgl_depth_lookup(ZBuffer* zb, GLfloat u, GLfloat v, GLfloat ref) {
	const GLfloat* depth = zb->depth_texture;
	GLint dim = zb->depth_texture_dim;
	GLint compare = (zb->depth_compare_mode == GL_COMPARE_R_TO_TEXTURE);
	GLint less = (zb->depth_compare_func == GL_LESS);

#define TGL_DEPTH_PASS(store) (less ? (ref < (store)) : (ref <= (store)))

	if (!zb->depth_linear) {
		GLint ix = (GLint)floorf(u * (GLfloat)dim);
		GLint iy = (GLint)floorf(v * (GLfloat)dim);
		GLfloat stored = tgl_depth_fetch(depth, dim, ix, iy);
		if (compare)
			return TGL_DEPTH_PASS(stored) ? 1.0f : 0.0f;
		return stored;
	} else {
		GLfloat gx = u * (GLfloat)dim - 0.5f;
		GLfloat gy = v * (GLfloat)dim - 0.5f;
		GLint x0 = (GLint)floorf(gx);
		GLint y0 = (GLint)floorf(gy);
		GLfloat fx = gx - (GLfloat)x0;
		GLfloat fy = gy - (GLfloat)y0;
		GLfloat w00 = (1.0f - fx) * (1.0f - fy);
		GLfloat w10 = fx * (1.0f - fy);
		GLfloat w01 = (1.0f - fx) * fy;
		GLfloat w11 = fx * fy;
		GLfloat s00 = tgl_depth_fetch(depth, dim, x0, y0);
		GLfloat s10 = tgl_depth_fetch(depth, dim, x0 + 1, y0);
		GLfloat s01 = tgl_depth_fetch(depth, dim, x0, y0 + 1);
		GLfloat s11 = tgl_depth_fetch(depth, dim, x0 + 1, y0 + 1);
		if (compare) {
			return (TGL_DEPTH_PASS(s00) ? w00 : 0.0f) + (TGL_DEPTH_PASS(s10) ? w10 : 0.0f) + (TGL_DEPTH_PASS(s01) ? w01 : 0.0f) +
				   (TGL_DEPTH_PASS(s11) ? w11 : 0.0f);
		}
		return s00 * w00 + s10 * w10 + s01 * w01 + s11 * w11;
	}
#undef TGL_DEPTH_PASS
}

static PIXEL tgl_depth_gray_pixel(GLfloat g) {
	GLint gray;
	if (g < 0.0f)
		g = 0.0f;
	else if (g > 1.0f)
		g = 1.0f;
	gray = (GLint)(g * 255.0f + 0.5f);
	if (gray > 255)
		gray = 255;
#if TGL_FEATURE_RENDER_BITS == 32
	return (PIXEL)((gray << 16) | (gray << 8) | gray);
#elif TGL_FEATURE_RENDER_BITS == 16
	{
		GLint c = gray >> 3;
		return (PIXEL)((c << 11) | (c << 5) | c);
	}
#endif
}

#define TGL_DEPTH_DRAW_LINE(BLEND_PIXEL)                                                                                                                         \
	{                                                                                                                                                             \
		register GLushort* pz;                                                                                                                                    \
		register PIXEL* pp;                                                                                                                                       \
		register GLuint z;                                                                                                                                        \
		register GLint n;                                                                                                                                         \
		register GLfloat sw, tw, rw, qw;                                                                                                                          \
		OR1OG1OB1DECL                                                                                                                                             \
		n = (x2 >> 16) - x1;                                                                                                                                      \
		pp = (PIXEL*)pp1 + x1;                                                                                                                                    \
		pz = pz1 + x1;                                                                                                                                            \
		z = z1;                                                                                                                                                   \
		sw = sw1;                                                                                                                                                 \
		tw = tw1;                                                                                                                                                 \
		rw = rw1;                                                                                                                                                 \
		qw = qw1;                                                                                                                                                 \
		or1 = r1;                                                                                                                                                 \
		og1 = g1;                                                                                                                                                 \
		ob1 = b1;                                                                                                                                                 \
		while (n >= 0) {                                                                                                                                          \
			GLfloat qinv = 1.0f / qw;                                                                                                                             \
			GLfloat u = sw * qinv;                                                                                                                                \
			GLfloat v = tw * qinv;                                                                                                                                \
			GLfloat ref = rw * qinv;                                                                                                                              \
			GLfloat g = tgl_depth_lookup(zb, u, v, ref);                                                                                                         \
			PIXEL tpix = tgl_depth_gray_pixel(g);                                                                                                                 \
			GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                                \
			if (ZCMPSIMP(zz, pz[0], 0, 0)) {                                                                                                                      \
				BLEND_PIXEL(RGB_MIX_FUNC(or1, og1, ob1, tpix), pp[0]);                                                                                           \
				if (zbdw)                                                                                                                                         \
					pz[0] = zz;                                                                                                                                   \
			}                                                                                                                                                     \
			z += dzdx;                                                                                                                                            \
			sw += dswdx;                                                                                                                                          \
			tw += dtwdx;                                                                                                                                          \
			rw += drwdx;                                                                                                                                          \
			qw += dqwdx;                                                                                                                                          \
			OR1G1B1INCR                                                                                                                                           \
			pz += 1;                                                                                                                                              \
			pp++;                                                                                                                                                 \
			n--;                                                                                                                                                  \
		}                                                                                                                                                         \
	}

#define TGL_SHADOW_RASTER_HEADER                                                                                                                                 \
	GLubyte zbdw = zb->depth_write;                                                                                                                              \
	GLubyte zbdt = zb->depth_test;                                                                                                                               \
	TGL_STIPPLEVARS

void ZB_fillTriangleDepthPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	TGL_BLEND_VARS
	TGL_SHADOW_RASTER_HEADER;
#define INTERP_Z
#define INTERP_RGB
#define INTERP_SHADOW
#undef OR1OG1OB1DECL
#undef OR1G1B1INCR
#define OR1OG1OB1DECL                                                                                                                                           \
	register GLint or1, og1, ob1;
#define OR1G1B1INCR                                                                                                                                             \
	og1 += dgdx;                                                                                                                                                 \
	or1 += drdx;                                                                                                                                                 \
	ob1 += dbdx;
#define DRAW_INIT()
#define PUT_PIXEL(_a)
#define DRAW_LINE()                                                                                                                                            \
	TGL_DEPTH_DRAW_LINE(TGL_BLEND_FUNC)
#include "ztriangle.h"
#undef OR1OG1OB1DECL
#undef OR1G1B1INCR
}

void ZB_fillTriangleDepthPerspectiveNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	TGL_SHADOW_RASTER_HEADER;
#define INTERP_Z
#define INTERP_RGB
#define INTERP_SHADOW
#undef OR1OG1OB1DECL
#undef OR1G1B1INCR
#define OR1OG1OB1DECL                                                                                                                                           \
	register GLint or1, og1, ob1;
#define OR1G1B1INCR                                                                                                                                             \
	og1 += dgdx;                                                                                                                                                 \
	or1 += drdx;                                                                                                                                                 \
	ob1 += dbdx;
#define DRAW_INIT()
#define PUT_PIXEL(_a)
#define DRAW_LINE()                                                                                                                                            \
	TGL_DEPTH_DRAW_LINE(TGL_NO_BLEND_FUNC)
#include "ztriangle.h"
#undef OR1OG1OB1DECL
#undef OR1G1B1INCR
}

#undef TGL_DEPTH_DRAW_LINE
#undef TGL_SHADOW_RASTER_HEADER
