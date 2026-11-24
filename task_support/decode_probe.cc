#include <libde265/de265.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
static std::vector<unsigned char> readfile(const char* p){std::ifstream f(p,std::ios::binary);return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});}
int main(int argc,char**argv){
 if(argc!=2&&argc!=3){std::fprintf(stderr,"usage: decode_probe bitstream [reference-yuv]\n");return 1;}
 auto input=readfile(argv[1]);if(input.empty())return 2;
 std::vector<unsigned char> expected;if(argc==3){expected=readfile(argv[2]);if(expected.empty())return 3;}
 de265_decoder_context* ctx=de265_new_decoder();if(!ctx)return 4;
 unsigned long long hash=1469598103934665603ULL;size_t bytes=0;int frames=0;
 auto drain=[&]()->int{
  while(const de265_image* img=de265_get_next_picture(ctx)){
   int planes=de265_get_chroma_format(img)==de265_chroma_mono?1:3;
   for(int c=0;c<planes;++c){int w=de265_get_image_width(img,c),h=de265_get_image_height(img,c),stride=0;
    const unsigned char* p=de265_get_image_plane(img,c,&stride);int depth=de265_get_bits_per_pixel(img,c);
    if(!p||w<=0||h<=0||depth<8||depth>16)return 5;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
     unsigned value;if(depth>8){unsigned short t;std::memcpy(&t,p+y*stride+2*x,2);value=t;}else value=p[y*stride+x];
     for(int b=0;b<(depth>8?2:1);++b){unsigned char v=(value>>(8*b))&255;
      if(argc==3&&(bytes>=expected.size()||expected[bytes]!=v)){
       std::fprintf(stderr,"pixel mismatch frame=%d plane=%d x=%d y=%d byte=%d offset=%zu actual=%u expected=%d\n",frames,c,x,y,b,bytes,(unsigned)v,bytes<expected.size()?(int)expected[bytes]:-1);return 6;
      }
      hash=(hash^v)*1099511628211ULL;++bytes;
     }
    }
   }
   ++frames;
  }
  return 0;
 };
 auto pump=[&]()->int{
  for(int iterations=0;iterations<10000;++iterations){int more=0;de265_error err=de265_decode(ctx,&more);int r=drain();if(r)return r;
   if(err==DE265_ERROR_WAITING_FOR_INPUT_DATA)return 0;
   if(err==DE265_ERROR_IMAGE_BUFFER_FULL)continue;
   if(!de265_isOK(err)){std::fprintf(stderr,"decode error %d %s\n",(int)err,de265_get_error_text(err));return 7;}
   if(!more)return 0;
  }return 8;
 };
 for(size_t at=0;at<input.size();){size_t n=std::min<size_t>(131,input.size()-at);
  if(de265_push_data(ctx,input.data()+at,(int)n,0,0)!=DE265_OK)return 9;at+=n;int r=pump();if(r)return r;
 }
 if(de265_flush_data(ctx)!=DE265_OK)return 10;int r=pump();if(r)return r;
 de265_free_decoder(ctx);
 if(frames==0||bytes==0||(argc==3&&(bytes!=expected.size()||frames!=3))){std::fprintf(stderr,"incomplete frames=%d bytes=%zu reference=%zu\n",frames,bytes,expected.size());return 11;}
 std::printf("decode frames=%d bytes=%zu hash=%016llx\n",frames,bytes,hash);return 0;
}
