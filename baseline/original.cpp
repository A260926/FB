#include "../src/pugixml.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
struct alignas(std::max_align_t) Header { std::size_t size; };
static std::size_t live = 0;
static void* allocate(std::size_t n) { Header* h = static_cast<Header*>(std::malloc(sizeof(Header)+n)); if (!h) return nullptr; h->size=n; live+=n; return h+1; }
static void release(void* p) { if (p) { Header* h=static_cast<Header*>(p)-1; live-=h->size; std::free(h); } }
static void check(bool v, const char* label) { if (!v) { std::cerr << "FAIL " << label << '\n'; std::exit(1); } }
int main() {
 pugi::set_memory_management_functions(allocate,release);
 {
  pugi::xml_document doc, kept;
  std::istringstream stream("<feed><item id='a'>&#x1F600;&amp;\r\nx<![CDATA[<raw>]]><leaf /></item><item id='b'>two</item></feed>");
  check(bool(doc.load(stream)),"original stream parse");
  auto item=doc.child("feed").child("item");
  check(std::strcmp(item.child_value(),"\xF0\x9F\x98\x80&\nx")==0,"entity and EOL");
  check(item.first_child().next_sibling().type()==pugi::node_cdata,"CDATA type");
  check(std::strcmp(item.first_child().next_sibling().value(),"<raw>")==0,"CDATA value");
  check(item.next_sibling().attribute("id").value()==std::string("b"),"record order");
  kept.append_copy(item); doc.reset();
  check(kept.child("item").child("leaf") && kept.child("item").attribute("id").value()==std::string("a"),"independent copy");
  std::cout << "ORIGINAL_STREAM_CONTENT_OK\nINDEPENDENT_COPY_OK\n";
 }
 check(live==0,"small parse freed");
 {
  std::string xml="<feed>"; const std::string row="<item>"+std::string(512,'x')+"</item>";
  for (int i=0;i<8192;++i) xml+=row;
  xml+="</feed>";
  pugi::xml_document doc;
  check(bool(doc.load_buffer(xml.data(),xml.size())),"large original parse");
  std::size_t before=live; doc.child("feed").remove_children();
  check(!doc.child("feed").first_child(),"children removed");
  check(live>=xml.size(),"source buffer remains despite child deletion");
  std::cout << "SOURCE_BUFFER_RETAINED_AFTER_REMOVE input=" << xml.size() << " before=" << before << " after=" << live << '\n';
  doc.reset(); check(live==0,"reset releases input");
 }
 std::cout << "ORIGINAL_BASELINE_OK\n";
}
