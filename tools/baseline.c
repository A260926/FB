#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <libfdt.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"baseline check line %d\n",__LINE__);exit(1); } } while(0)
int main(void){
 char blob[4096];int bus,dev,len;const fdt32_t *reg;fdt32_t ranges[]={cpu_to_fdt32(0),cpu_to_fdt32(0),cpu_to_fdt32(0x40000000),cpu_to_fdt32(0x1000)};fdt32_t data[]={cpu_to_fdt32(0x180),cpu_to_fdt32(0x40)};
 CHECK(!fdt_create_empty_tree(blob,sizeof(blob)));CHECK(!fdt_setprop_u32(blob,0,"#address-cells",2));CHECK(!fdt_setprop_u32(blob,0,"#size-cells",1));bus=fdt_add_subnode(blob,0,"soc");CHECK(bus>=0);CHECK(!fdt_setprop_u32(blob,bus,"#address-cells",1));CHECK(!fdt_setprop_u32(blob,bus,"#size-cells",1));CHECK(!fdt_setprop(blob,bus,"ranges",ranges,sizeof(ranges)));dev=fdt_add_subnode(blob,bus,"device@180");CHECK(dev>=0);CHECK(!fdt_setprop(blob,dev,"reg",data,sizeof(data)));CHECK(!fdt_check_full(blob,sizeof(blob)));
 bus=fdt_path_offset(blob,"/soc");dev=fdt_path_offset(blob,"/soc/device@180");reg=fdt_getprop(blob,dev,"reg",&len);CHECK(reg&&len==8);CHECK(fdt_getprop(blob,bus,"ranges",&len)&&len==16);
 printf("LIBFDT_BASELINE_OK cells=%d/%d reg=%#x size=%#x ranges_bytes=%d\n",fdt_address_cells(blob,bus),fdt_size_cells(blob,bus),fdt32_to_cpu(reg[0]),fdt32_to_cpu(reg[1]),len);return 0;
}
