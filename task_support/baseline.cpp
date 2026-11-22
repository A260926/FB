#include "libde265/de265.h"
#include "libde265/image.h"
#include "libde265/slice.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#define REQUIRE(x) do { if(!(x)) { std::fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
struct Result { int frames=0,b=0,tmvp=0,multi=0; unsigned long long hash=14695981039346656037ull; };
static int run(int workers, const std::vector<unsigned char>& bytes,Result& result){
 de265_decoder_context* d=de265_new_decoder(); REQUIRE(d);
 if(workers) REQUIRE(de265_start_worker_threads(d,workers)==DE265_OK);
 REQUIRE(de265_push_data(d,bytes.data(),(int)bytes.size(),1234,nullptr)==DE265_OK);
 REQUIRE(de265_flush_data(d)==DE265_OK);
 int more=1,loops=0;
 while(more){
  REQUIRE(++loops<10000);more=0;de265_error e=de265_decode(d,&more);
  REQUIRE(e==DE265_OK || e==DE265_ERROR_WAITING_FOR_INPUT_DATA || e==DE265_ERROR_IMAGE_BUFFER_FULL);
  const de265_image* im;
  while((im=de265_get_next_picture(d))!=nullptr){
   REQUIRE(de265_get_image_PTS(im)==1234);result.frames++;
   bool b=false,t=false,m=false;
   for(const auto* sh:im->slices){b|=sh->slice_type==SLICE_TYPE_B;t|=sh->slice_temporal_mvp_enabled_flag;m|=sh->num_ref_idx_l0_active>1 || sh->num_ref_idx_l1_active>1;}
   result.b+=b;result.tmvp+=t;result.multi+=m;
   for(int c=0;c<3;c++) {int stride=0,w=de265_get_image_width(im,c),h=de265_get_image_height(im,c);const unsigned char* plane=de265_get_image_plane(im,c,&stride);REQUIRE(plane && w>0 && h>0);REQUIRE(de265_get_bits_per_pixel(im,c)==8);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){result.hash^=plane[y*stride+x];result.hash*=1099511628211ull;}}
  }
  if(e==DE265_ERROR_WAITING_FOR_INPUT_DATA)break;
 }
 REQUIRE(result.frames==75);REQUIRE(de265_free_decoder(d)==DE265_OK);
 std::printf("HEVC_SEQUENCE workers=%d frames=%d B=%d TMVP=%d multi_ref=%d pixel_fnv=%016llx\n",workers,result.frames,result.b,result.tmvp,result.multi,result.hash);return 0;
}
int main(){std::ifstream in("testdata/girlshy.h265",std::ios::binary);REQUIRE(in);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});REQUIRE(!bytes.empty());Result a,b;REQUIRE(run(0,bytes,a)==0);REQUIRE(run(2,bytes,b)==0);REQUIRE(a.hash==b.hash && a.b>0 && a.tmvp>0 && a.multi>0);std::puts("BASELINE_COMPLETE production=de265_decode pixels_workers_equal=1");return 0;}
