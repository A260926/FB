#ifndef TASK_SPC_FIXTURE_H
#define TASK_SPC_FIXTURE_H
#include <vector>
#include <cstring>
#include <algorithm>
#include "../gme/Spc_Emu.h"
#include "../gme/Data_Reader.h"
namespace fixture {
inline std::vector<unsigned char> spc(unsigned seed=1){
 std::vector<unsigned char>b(0x10200,0);
 const char sig[]="SNES-SPC700 Sound File Data v0.30\x1a\x1a";std::memcpy(b.data(),sig,sizeof(sig)-1);
 b[0x23]=26;b[0x24]=30;b[0x25]=0;b[0x26]=2;b[0x2b]=0xef;
 unsigned char*r=b.data()+0x100;unsigned char*d=b.data()+0x10100;
 // Timer0 and CPU reads of input ports 0/1 control voice0 pitch.
 const unsigned char code[]={0xe4,0xfd,0xf0,0x02,0xab,0x10,0xe4,0xf4,0x44,0xf5,0x44,0x10,0x8f,0x02,0xf2,0xc4,0xf3,0x2f,0xed};
 std::memcpy(r+0x200,code,sizeof(code));r[0xf1]=1;r[0xfa]=17;r[0x10]=seed*13;
 // One directory entry, four self-generated BRR blocks, last one loops.
 r[0x3000]=0;r[0x3001]=0x40;r[0x3002]=0;r[0x3003]=0x40;
 for(unsigned j=0;j<4;j++){
  r[0x4000+j*9]=0x80|(j==3?3:0);
  for(unsigned i=0;i<8;i++){unsigned a=(i*3+j*5+seed)%16;unsigned z=(i*7+j*3+seed*2)%16;r[0x4001+j*9+i]=(a<<4)|z;}
 }
 d[0]=100;d[1]=67;d[2]=0;d[3]=0x10;d[4]=0;d[5]=0;d[7]=0x7f;
 d[0x0c]=110;d[0x1c]=97;d[0x2c]=35;d[0x3c]=27;d[0x0d]=36;
 d[0x4c]=1;d[0x5c]=0;d[0x6c]=0;d[0x4d]=1;d[0x5d]=0x30;d[0x6d]=0x60;d[0x7d]=1;d[0x0f]=100;
 return b;
}
inline const char* setup(Spc_Emu&e,const std::vector<unsigned char>&b,long rate){
 const char*err=e.set_sample_rate(rate);if(err)return err;e.ignore_silence(true);e.set_autoload_playback_limit(false);e.enable_accuracy(true);
 Mem_File_Reader reader(b.data(),long(b.size()));err=e.load(reader);if(err)return err;return e.start_track(0);
}
inline const char* render(Spc_Emu&e,short*out,long frames,long chunk){
 for(long pos=0;pos<frames;){long n=std::min(chunk,frames-pos);const char*err=e.play(2*n,out+2*pos);if(err)return err;pos+=n;}return 0;
}
}
#endif
