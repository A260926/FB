#include "mikktspace_edit.h"
#include <math.h>
#include <string.h>
int main(void){
 MikkEditFace f={0};f.count=3;f.position[1][0]=1;f.position[2][1]=1;f.uv[1][0]=1;f.uv[2][1]=1;for(int k=0;k<3;k++)f.normal[k][2]=1;
 MikkEditSession*s=mikk_edit_create(1,&f,180,0);if(!s)return 31;
 MikkEditTangent t={0};if(!mikk_edit_get(s,0,0,&t)||fabs(t.tangent[0]-1)>1e-6||fabs(t.bitangent[1]-1)>1e-6||fabs(t.mag_s-1)>1e-6||fabs(t.mag_t-1)>1e-6||!t.orientation)return 32;
 MikkEditUpdate u={0};u.value=f;u.value.uv[1][0]=2;u.value.uv[2][1]=2;
 if(!mikk_edit_update(s,&u,1)||!mikk_edit_get(s,0,0,&t)||fabs(t.mag_s-.5)>1e-6||fabs(t.mag_t-.5)>1e-6)return 33;
 const MikkEditStats*d=mikk_edit_last_update(s);if(!d||!d->evaluated_count||!d->published_count)return 34;
 if(!mikk_edit_update(s,0,0))return 35;d=mikk_edit_last_update(s);if(!d||d->evaluated_count||d->published_count)return 36;
 mikk_edit_free(s);return 0;
}
