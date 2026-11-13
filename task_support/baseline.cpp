#include "Ap4.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4ElstAtom.h"
#include "Ap4FileWriter.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"baseline assertion line %d\n",__LINE__); return 1; } } while(0)
int main() {
    const AP4_UI08 bytes[] = {10,11,12,13,14,15,16,17};
    AP4_MemoryByteStream* input = new AP4_MemoryByteStream(bytes,8);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    CHECK(AP4_SUCCEEDED(table->AddSampleDescription(new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,AP4_ATOM_TYPE('r','a','w',' '),NULL))));
    const AP4_UI32 delta[] = {200,400,100,100,200,400,100,100};
    for (unsigned i=0;i<8;++i) CHECK(AP4_SUCCEEDED(table->AddSample(*input,i,1,100,0,i*100,delta[i],i%4==0)));
    input->Release();
    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_VIDEO,table,1,1000,800,1000,1000,"und",0,0);
    AP4_ContainerAtom* edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
    AP4_ElstAtom* elst = new AP4_ElstAtom();
    CHECK(AP4_SUCCEEDED(elst->AddEntry(AP4_ElstEntry(100,-1,1))));
    CHECK(AP4_SUCCEEDED(elst->AddEntry(AP4_ElstEntry(700,200,1))));
    CHECK(AP4_SUCCEEDED(edts->AddChild(elst)));
    CHECK(AP4_SUCCEEDED(const_cast<AP4_TrakAtom*>(track->GetTrakAtom())->AddChild(edts)));
    AP4_Result status = AP4_SUCCESS;
    AP4_Track* clone = track->Clone(&status);
    CHECK(AP4_SUCCEEDED(status) && clone && clone->GetSampleCount()==8);
    delete clone;
    CHECK(track->GetNearestSyncSampleIndex(3)==0);
    AP4_Ordinal index=99;
    CHECK(track->GetSampleIndexForTimeStampMs(150,index)==AP4_ERROR_NOT_SUPPORTED);
    AP4_Movie* movie = new AP4_Movie(1000);
    CHECK(AP4_SUCCEEDED(movie->AddTrack(track)));
    AP4_File file(movie);
    AP4_MemoryByteStream* encoded = new AP4_MemoryByteStream();
    CHECK(AP4_SUCCEEDED(AP4_FileWriter::Write(file,*encoded)));
    CHECK(AP4_SUCCEEDED(encoded->Seek(0)));
    {
        AP4_File reread(*encoded);
        AP4_Track* t = reread.GetMovie()->GetTrack(1);
        CHECK(t && t->GetSampleCount()==8 && t->GetDuration()==800);
        CHECK(AP4_SUCCEEDED(t->GetSampleIndexForTimeStampMs(150,index)));
        std::printf("MEDIA_LOOKUP parsed_index=%u (unprojected media timestamp)\n",index);
        for (unsigned i=0;i<8;++i) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            CHECK(AP4_SUCCEEDED(t->ReadSample(i,sample,data)));
            CHECK(data.GetDataSize()==1 && data.GetData()[0]==bytes[i]);
            CHECK(sample.GetDts()==i*100 && sample.GetCtsDelta()==delta[i]);
        }
        AP4_ElstAtom* e = AP4_DYNAMIC_CAST(AP4_ElstAtom,const_cast<AP4_TrakAtom*>(t->GetTrakAtom())->FindChild("edts/elst"));
        CHECK(e && e->GetEntries().ItemCount()==2);
        CHECK(e->GetEntries()[0].m_MediaTime==-1 && e->GetEntries()[1].m_MediaTime==200);
    }
    encoded->Release();
    std::puts("BENTO4_BASELINE samples=8 clone=8 sync_before_3=0 edit_entries=2 payload_roundtrip=ok");
    return 0;
}
