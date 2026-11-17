#include "marrow.h"
#include "convert.h"
#include "mrw_authoring.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int fixture(const char *p, int forest) {
 FILE *f=fopen(p,"wb"); if(!f)return 1;
 fprintf(f,"{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"name\":\"A\",\"translation\":[2,0,0]%s},{\"name\":\"B\",\"translation\":[0,3,0]}],\"skins\":[{\"joints\":[1,0]}]}",forest?"":",\"children\":[1]");
 return fclose(f)!=0;
}
int main(void) {
 const char *path=".task-build/baseline.gltf";
 if(fixture(path,0))return 1;
 mrw_g2m_options o={0};o.input_path=path;o.fps=30;o.skin_index=-1;
 unsigned char *buf=0;size_t n=0;char diag[256]={0};
 mrw_result rc=mrw_g2m_convert(&o,&buf,&n,diag,sizeof diag);
 if(rc!=MRW_OK){fprintf(stderr,"single: %u %s\n",rc,diag);return 2;}
 mrw_blob b;mrw_skeleton_view s;mrw_trs local[2];float model[24],pal[24];
 if(mrw_blob_open(buf,n,&b)||mrw_blob_skeleton(&b,&s)||s.joint_count!=2)return 3;
 for(unsigned j=0;j<2;j++)if(mrw_skeleton_rest_local(&s,j,local+j))return 4;
 if(mrw_local_to_model(&s,local,model,2)||mrw_model_to_palette(&s,model,pal,2))return 5;
 if(fabsf(pal[15]-2)>1e-5f||fabsf(pal[19]-3)>1e-5f)return 6;
 printf("SINGLE_TREE_PALETTE_OK joints=%u child=(%.1f,%.1f)\n",s.joint_count,pal[15],pal[19]);
 mrw_authoring_free(buf);buf=0;n=0;
 if(fixture(path,1))return 7;
 rc=mrw_g2m_convert(&o,&buf,&n,diag,sizeof diag);
 printf("FOREST_CONVERT rc=%u diagnostic=%s\n",rc,diag);
 if(buf)mrw_authoring_free(buf);buf=0;n=0;
 FILE *f=fopen(path,"wb");if(!f)return 8;
 fputs("{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"name\":\"scale_helper\",\"scale\":[2,1,1],\"children\":[1]},{\"name\":\"joint\",\"rotation\":[0,0,0.3826834324,0.9238795325]}],\"skins\":[{\"joints\":[1]}]}",f);
 if(fclose(f))return 9;
 rc=mrw_g2m_convert(&o,&buf,&n,diag,sizeof diag);
 printf("SHEAR_FOLD_CONVERT rc=%u diagnostic=%s\n",rc,diag);
 if(buf)mrw_authoring_free(buf);
 return 0;
}
