#include <stdio.h>
#include <string.h>
int main(int argc,char** argv){
 if(argc!=2)return 2;FILE* f=fopen(argv[1],"rb");if(!f)return 3;
 char line[80];const char* header[]={"P6\n","64 64\n","255\n"};
 for(int i=0;i<3;++i)if(!fgets(line,sizeof(line),f)||strcmp(line,header[i]))return 4;
 unsigned char first[3],px[3];if(fread(first,1,3,f)!=3)return 5;
 unsigned changed=0,nonzero=(first[0]||first[1]||first[2]);
 for(int i=1;i<4096;++i){if(fread(px,1,3,f)!=3)return 5;changed+=memcmp(px,first,3)!=0;nonzero+=(px[0]||px[1]||px[2]);}
 if(fgetc(f)!=EOF||!changed||!nonzero)return 6;fclose(f);
 printf("GEOMETRY_EXAMPLE_PPM_OK pixels=4096 varied=%u nonzero=%u\n",changed,nonzero);return 0;
}
