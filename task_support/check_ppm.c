#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc,char **argv){
 if(argc!=2)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;
 char m[3]={0};unsigned long w,h,max;
 if(fscanf(f,"%2s %lu %lu %lu",m,&w,&h,&max)!=4||m[0]!='P'||m[1]!='6'||!w||!h||w>16384||h>16384||max!=255)return 4;
 int c=fgetc(f);if(c!='\n'&&c!=' '&&c!='\r'&&c!='\t')return 5;
 size_t n=(size_t)w*h*3;for(size_t i=0;i<n;i++)if(fgetc(f)==EOF)return 6;
 if(fgetc(f)!=EOF||ferror(f))return 7;fclose(f);
 printf("EXAMPLE_P6 %lux%lu bytes=%zu\n",w,h,n);return 0;
}
