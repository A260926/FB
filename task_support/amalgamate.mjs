import {readFileSync, mkdirSync, writeFileSync} from 'node:fs';
const get=n=>readFileSync('src/'+n,'utf8');
const decl=['header_macros_start.h','buffer_format_macros.h','crsw_math.h','gl_types.h','cvector_combined.h','glcontext.h','gl_glsl.h','pgl_std_shaders.h','gl_prototypes.h','gl_stubs.h','pgl_ext.h'];
const impl=['crsw_math.c','cvector_combined.c','gl_err.c','gl_internal.c','gl_tex_internal.c','gl_fbo.c','gl_impl.c','gl_stubs.c','gl_glsl.c','pgl_ext.c','pgl_std_shaders.c'];
const text='/*\n'+get('header_docs.txt')+'*/\n'+decl.map(get).join('')+'\n#ifdef __cplusplus\n}\n#endif\n#endif\n#ifdef PORTABLEGL_IMPLEMENTATION\n'+impl.map(get).join('')+'\n#undef PORTABLEGL_IMPLEMENTATION\n#endif\n'+get('close_pgl.h');
mkdirSync('.task-build',{recursive:true});writeFileSync('.task-build/portablegl.h',text);
