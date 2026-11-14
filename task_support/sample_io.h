#ifndef SAMPLE_IO_H
#define SAMPLE_IO_H
#include <libpe/pe.h>
#include <stdlib.h>
#include <string.h>
static int sample_open(pe_ctx_t *ctx,const char *path){
 memset(ctx,0,sizeof(*ctx));FILE *f=fopen(path,"rb");if(!f)return 1;
 if(fseek(f,0,SEEK_END)){fclose(f);return 2;}long n=ftell(f);rewind(f);if(n<=0){fclose(f);return 3;}
 ctx->map_addr=malloc((size_t)n);if(!ctx->map_addr){fclose(f);return 4;}
 if(fread(ctx->map_addr,1,(size_t)n,f)!=(size_t)n){free(ctx->map_addr);fclose(f);return 5;}fclose(f);
 ctx->map_size=n;ctx->map_end=(uintptr_t)ctx->map_addr+(size_t)n;
 return pe_parse(ctx)==LIBPE_E_OK?0:6;
}
static void sample_close(pe_ctx_t *ctx){free(ctx->pe.sections);free(ctx->pe.directories);free(ctx->map_addr);memset(ctx,0,sizeof(*ctx));}
#endif
