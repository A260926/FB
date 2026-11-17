#include "src/include/frame.hpp"
#include "src/include/file/sela_file.hpp"
#include "src/include/file/wav_file.hpp"
#include "src/include/sela/encoder.hpp"
#include "src/include/sela/decoder.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdint>
#include <vector>
static uint64_t fingerprint(const char*p){std::ifstream f(p,std::ios::binary);uint64_t h=14695981039346656037ULL;char c;while(f.get(c)){h^=(unsigned char)c;h*=1099511628211ULL;}return h;}
int main(){
 std::vector<std::vector<int32_t>> samples(3,std::vector<int32_t>(2048));
 for(size_t i=0;i<2048;++i) for(size_t c=0;c<3;++c) samples[c][i]=(int32_t)(12000*std::sin(i*0.073)+5000*std::cos(i*0.021));
 data::WavFrame original(16,samples);frame::FrameEncoder enc(original);auto compressed=enc.process();frame::FrameDecoder dec(compressed);auto back=dec.process();
 if(back.samples!=samples)return 2;
 std::vector<data::SelaFrame> frames;frames.push_back(compressed);
 file::SelaFile sf(44100,16,3,std::move(frames));{std::ofstream o(".task-build/legacy.sela",std::ios::binary);sf.writeToFile(o);if(!o)return 3;}
 file::SelaFile loaded;{std::ifstream i(".task-build/legacy.sela",std::ios::binary);loaded.readFromFile(i);}
 if(loaded.selaFrames.size()!=1||frame::FrameDecoder(loaded.selaFrames[0]).process().samples!=samples)return 4;
 std::vector<data::WavFrame> wav;wav.push_back(original);file::WavFile wf(44100,16,3,std::move(wav));{std::ofstream o(".task-build/input.wav",std::ios::binary);wf.writeToFile(o);}
 {std::ifstream i(".task-build/input.wav",std::ios::binary);auto fromFile=sela::Encoder(i).process();std::ofstream o(".task-build/file.sela",std::ios::binary);fromFile.writeToFile(o);}
 {std::ifstream i(".task-build/file.sela",std::ios::binary);auto out=sela::Decoder(i).process();if(out.wavChunk.dataSubChunk.wavFrames.size()!=1||out.wavChunk.dataSubChunk.wavFrames[0].samples!=samples)return 5;}
 auto h=fingerprint(".task-build/legacy.sela");std::ifstream reference("task_support/legacy-fnv.txt");uint64_t expected=0;if(!(reference>>expected)||h!=expected){std::cerr<<"legacy fingerprint changed or missing\n";return 6;}
 size_t roots=0,bytes=0;for(const auto&s:compressed.subFrames){roots+=s.subFrameType==0;bytes+=12+4*(s.encodedReflectionCoefficients.size()+s.encodedResidues.size());}
 std::cout<<"BASELINE_OK channels=3 samples=2048 frame_and_file_PCM_exact=1 legacy_fnv="<<h<<" roots="<<roots<<" payload_bytes="<<bytes<<"\n";return 0;
}
