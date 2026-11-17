#include <MachO/File.hpp>
#include <MachO/LoadCommands/Thread.hpp>
#include <MachO/LoadCommands/EntryPoint.hpp>
#include <MachO/LoadCommands/Segment64.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
static void put(std::vector<uint8_t>& b, uint64_t n, unsigned width) {
    for (unsigned i=0;i<width;i++) b.push_back(static_cast<uint8_t>(n>>(8*i)));
}
int main() {
    try {
        std::vector<uint8_t> b;
        for (uint32_t n : {0xfeedfacfu,0x1000007u,3u,2u,3u,280u,0u,0u}) put(b,n,4);
        put(b,5,4); put(b,184,4); put(b,4,4); put(b,42,4);
        for (unsigned i=0;i<21;i++) put(b,i==16 ? 0x100000810ull : (i==7 ? 0x7000ull : i),8);
        put(b,0x80000028,4); put(b,24,4); put(b,0x810,8); put(b,0x4000,8);
        put(b,0x19,4); put(b,72,4);
        const char seg[16]="__TEXT"; b.insert(b.end(),seg,seg+16);
        for (uint64_t n : {0x100000000ull,0x1000ull,0ull,0x1000ull}) put(b,n,8);
        for (uint32_t n : {7u,5u,0u,0u}) put(b,n,4);
        b.resize(0x1000,0);
        XS::IO::BinaryDataStream stream(b); MachO::File file(stream);
        auto threads=file.loadCommands<MachO::LoadCommands::Thread>();
        auto main=file.loadCommands<MachO::LoadCommands::EntryPoint>();
        auto segments=file.loadCommands<MachO::LoadCommands::Segment64>();
        if (file.loadCommands().size()!=3 || threads.size()!=1 || threads[0].size()!=184 || main.size()!=1 || main[0].offset()!=0x810 || segments.size()!=1 || segments[0].vmAddress()!=0x100000000ull) throw std::runtime_error("parse mismatch");
        std::cout << "BASELINE_OK commands=3 thread_size=184 main_fileoff=0x810 segment_va=0x100000000\n";
        std::cout << "CURRENT_THREAD_DATA_ITEMS=" << threads[0].data().size() << "\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
