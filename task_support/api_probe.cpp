// Fixed normal consumer, not a production implementation.
#include "Ap4.h"
#include "Ap4Mpeg2Ts.h"
#include "Ap4EditedTs.h"
#include "ts_check.h"
int main(){
 AP4_Mpeg2TsWriter::Stream s(0x102);AP4_MemoryByteStream* b=new AP4_MemoryByteStream();
 AP4_Result z=s.WritePcrPacket(300000,*b);
 if(AP4_FAILED(z)||b->GetDataSize()!=188||((b->GetData()[3]>>4)&3)!=2||read_pcr(b->GetData()+6)!=300000)return 21;b->Release();
 AP4_ByteStream* input=nullptr;
 if(AP4_FAILED(AP4_FileByteStream::Create(".task-build/simple.mp4",AP4_FileByteStream::STREAM_MODE_READ,input)))return 22;
 int rc;
 {AP4_File f(*input);if(!f.GetMovie())return 23;b=new AP4_MemoryByteStream();
 z=AP4_EditedTsWriter::Write(*f.GetMovie(),*b,100000,3600);
 rc=AP4_FAILED(z)?24:inspect_simple(b->GetData(),b->GetDataSize(),true);b->Release();}
 input->Release();return rc;
}
