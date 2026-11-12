#include "pugixml.hpp"
#include <cassert>
#include <string>
#include <vector>
#include <iostream>
struct Sink:pugi::xml_writer {std::string bytes;void write(const void*p,size_t n)override{bytes.append(static_cast<const char*>(p),n);}};
static std::string expected(pugi::xml_encoding enc) {
 const unsigned cps[]={'<','r','>','A',0x1f642,'B','<','/','r','>'};std::string b;
 for(unsigned c:cps){if(enc==pugi::encoding_utf8){if(c<128)b+=char(c);else b+="\xf0\x9f\x99\x82";continue;}
 bool le=enc==pugi::encoding_utf16_le||enc==pugi::encoding_utf32_le;bool utf16=enc==pugi::encoding_utf16_le||enc==pugi::encoding_utf16_be;
 std::vector<unsigned> words;if(utf16&&c>0xffff){c-=0x10000;words={0xd800+(c>>10),0xdc00+(c&1023)};}else words={c};
 for(unsigned w:words)for(int i=0;i<(utf16?2:4);i++)b+=char(w>>(8*(le?i:(utf16?1:3)-i)));
 }return b;
}
int main(){pugi::xml_document doc;auto root=doc.append_child(PUGIXML_TEXT("r"));
 root.append_child(pugi::node_pcdata).set_value(PUGIXML_TEXT("A\U0001f642B"));
 for(auto enc:{pugi::encoding_utf8,pugi::encoding_utf16_le,pugi::encoding_utf16_be,pugi::encoding_utf32_le,pugi::encoding_utf32_be}){
  Sink out;doc.save(out,PUGIXML_TEXT(""),pugi::format_raw|pugi::format_no_declaration,enc);assert(out.bytes==expected(enc));
  std::cout<<"ENCODING_"<<int(enc)<<"_PASS\n";
 }
 std::cout<<"PUGI_ENCODING_BASELINE_OK\n";
}
