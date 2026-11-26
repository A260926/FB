#include <libde265/de265.h>
#include <fstream>
#include <vector>
#include <iterator>
int main(int c,char**v){std::ifstream in(v[1],std::ios::binary);std::vector<unsigned char>d((std::istreambuf_iterator<char>(in)),{});auto*x=de265_new_decoder();if(c>3)de265_start_worker_threads(x,atoi(v[3]));de265_push_data(x,d.data(),d.size(),0,0);de265_flush_data(x);std::ofstream out(v[2],std::ios::binary);int more=1;while(more){auto e=de265_decode(x,&more);const de265_image*im;while((im=de265_get_next_picture(x))){for(int cx=0;cx<(de265_get_chroma_format(im)==de265_chroma_mono?1:3);cx++){int st;auto*p=de265_get_image_plane(im,cx,&st);int w=de265_get_image_width(im,cx),h=de265_get_image_height(im,cx);int dep=de265_get_bits_per_pixel(im,cx);for(int y=0;y<h;y++)out.write((char*)(p+y*st),w*((dep+7)/8));}}if(e==DE265_ERROR_WAITING_FOR_INPUT_DATA)break;if(!de265_isOK(e))return 2;}de265_free_decoder(x);}
