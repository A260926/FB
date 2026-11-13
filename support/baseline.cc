#include "common/vp9_header_parser.h"
#include "common/libwebm_util.h"
#include <vector>
#include <cstdio>
#include <cstring>
struct Bits {std::vector<unsigned char> b;unsigned n=0;void put(unsigned v,unsigned k){while(k--){if(n%8==0)b.push_back(0);b.back()|=((v>>k)&1)<<(7-n%8);n++;}}};
static std::vector<unsigned char> header(bool seg){
 Bits b;b.put(2,2);b.put(0,2);b.put(0,1);b.put(0,1);b.put(1,1);b.put(0,1);
 b.put(0x498342,24);b.put(1,3);b.put(0,1);b.put(511,16);b.put(255,16);b.put(0,1);
 b.put(1,1);b.put(0,1);b.put(0,2);b.put(0,6);b.put(0,3);b.put(0,1);
 b.put(0,8);b.put(0,1);b.put(0,1);b.put(0,1);b.put(seg,1);
 if(seg){b.put(1,1);for(int i=0;i<7;i++)b.put(0,1);b.put(1,1);for(int i=0;i<3;i++)b.put(0,1);b.put(1,1);b.put(0,1);for(int i=0;i<32;i++){b.put(i==0,1);if(i==0){b.put(2,8);b.put(1,1);}}}
 b.put(1,1);b.put(1,1);b.put(1,1); // 2 tile columns, 4 rows.
 b.put(1,16);while(b.n%8)b.put(0,1);b.put(0,8); // opaque compressed-header byte
 for(int i=0;i<8;i++){if(i<7)b.put(1,32);b.put(0x20+i,8);}return b.b;
}
int main(int argc,char**argv){
 vp9_parser::Vp9HeaderParser p;auto ordinary=header(false), segmented=header(true);
 if(!p.ParseUncompressedHeader(ordinary.data(),ordinary.size())||p.width()!=512||p.row_tiles()!=4||p.column_tiles()!=2)return 2;
 std::puts("VP9_BASELINE plain=512x256 tiles=2x4");
 vp9_parser::Vp9HeaderParser q;if(!q.ParseUncompressedHeader(segmented.data(),segmented.size()))return 3;
 std::printf("VP9_SEGMENTATION observed_tiles=%dx%d expected=2x4\n",q.column_tiles(),q.row_tiles());
 if(argc>1&&!std::strcmp(argv[1],"--require-layout")&&(q.column_tiles()!=2||q.row_tiles()!=4))return 9;
 libwebm::Ranges ranges;bool error=false;unsigned char indexed[]={1,2,3,4,5,6,0xc0,6,0xc0};
 if(!libwebm::ParseVP9SuperFrameIndex(indexed,sizeof indexed,&ranges,&error)||error||ranges.size()!=1)return 4;
 std::puts("VP9_INDEX_BASELINE child_length=6");return 0;
}
