#include "PDB_PCH.h"
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_DBIStream.h"
#include "PDB_ImageSectionStream.h"
#include "PDB_SectionContributionStream.h"
#include "msf_fixture.h"
#include <cstdio>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"baseline failed: %s:%d %s\n",__FILE__,__LINE__,#x);return 1;} } while(0)
int main() {
    using fixture::Bytes;
    std::vector<Bytes> streams(9);
    PDB::DBI::StreamHeader h{};
    h.signature=PDB::DBI::StreamHeader::Signature;h.version=PDB::DBI::StreamHeader::Version::V70;
    h.globalStreamIndex=h.publicStreamIndex=h.symbolRecordStreamIndex=0xffff;
    h.sectionContributionSize=4+sizeof(PDB::DBI::SectionContribution);
    h.sectionMapSize=4;h.sourceInfoSize=4;h.optionalDebugHeaderSize=sizeof(PDB::DBI::DebugHeader);
    fixture::append(streams[3],h);
    fixture::append(streams[3],PDB::DBI::SectionContribution::Version::Ver60);
    PDB::DBI::SectionContribution contribution{};
    contribution.section=1;contribution.offset=4;contribution.size=8;contribution.moduleIndex=7;
    fixture::append(streams[3],contribution);
    fixture::u32(streams[3],0);fixture::u32(streams[3],0);
    PDB::DBI::DebugHeader dbg;std::memset(&dbg,0xff,sizeof(dbg));
    dbg.sectionHeaderStreamIndex=5;dbg.originalSectionHeaderDataStreamIndex=6;dbg.omapFromSrcDataStreamIndex=7;
    fixture::append(streams[3],dbg);
    PDB::IMAGE_SECTION_HEADER section{};std::memcpy(section.Name,".text",5);
    section.VirtualAddress=0x5000;section.Misc.VirtualSize=0x100;section.SizeOfRawData=0x100;
    fixture::append(streams[5],section);
    section.VirtualAddress=0x1000;fixture::append(streams[6],section);
    fixture::u32(streams[7],0x1000);fixture::u32(streams[7],0x5000);
    fixture::u32(streams[7],0x1010);fixture::u32(streams[7],0);
    streams[8].resize(1100);for (size_t i=0;i<streams[8].size();++i) streams[8][i]=static_cast<unsigned char>((i*37+19)&255);
    const auto bytes=fixture::msf(streams);
    REQUIRE(PDB::ValidateFile(bytes.data(),bytes.size())==PDB::ErrorCode::Success);
    auto raw=PDB::CreateRawFile(bytes.data());
    REQUIRE(raw.GetStreamCount()==9);
    auto scattered=raw.CreateMSFStream<PDB::DirectMSFStream>(8);
    Bytes read(1030);scattered.ReadAtOffset(read.data(),read.size(),29);
    REQUIRE(std::equal(read.begin(),read.end(),streams[8].begin()+29));
    std::puts("MSF_SCATTER_READ_OK bytes=1030");
    REQUIRE(PDB::HasValidDBIStream(raw)==PDB::ErrorCode::Success);
    auto dbi=PDB::CreateDBIStream(raw);
    REQUIRE(dbi.HasValidImageSectionStream(raw)==PDB::ErrorCode::Success);
    REQUIRE(dbi.HasValidSectionContributionStream(raw)==PDB::ErrorCode::Success);
    auto image=dbi.CreateImageSectionStream(raw);
    REQUIRE(image.ConvertSectionOffsetToRVA(1,4)==0x5004);
    REQUIRE(image.ConvertSectionOffsetToRVA(0,4)==0);
    REQUIRE(image.ConvertSectionOffsetToRVA(2,4)==0);
    // Retain the owning stream while examining its view.
    auto ownedContributions=dbi.CreateSectionContributionStream(raw);
    auto view=ownedContributions.GetContributions();
    REQUIRE(view.GetLength()==1 && view[0].moduleIndex==7 && view[0].offset==4 && view[0].size==8);
    std::puts("DBI_IMAGE_CONTRIBUTIONS_OK current_rva=0x5004 module=7");
    PDB::ImageSectionStream original(raw,6);
    REQUIRE(original.ConvertSectionOffsetToRVA(1,4)==0x1004);
    auto from=raw.CreateMSFStream<PDB::DirectMSFStream>(7);
    REQUIRE(from.GetSize()==16);
    REQUIRE(from.ReadAtOffset<uint32_t>(0)==0x1000 && from.ReadAtOffset<uint32_t>(4)==0x5000);
    REQUIRE(from.ReadAtOffset<uint32_t>(8)==0x1010 && from.ReadAtOffset<uint32_t>(12)==0);
    std::puts("RAW_OMAP_BYTES_OK rows=2");
    std::puts("ADDRESS_PROVENANCE_BASELINE_OK");
}
