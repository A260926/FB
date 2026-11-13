#include "font_fixture.hpp"
#include <cstdio>
#include <cstdlib>
#define REQUIRE(c) do { if(!(c)){std::fprintf(stderr,"baseline failed: %s\n",#c);std::exit(1);} }while(0)
int main() {
 using namespace text_path_fixture; REQUIRE(install());
 auto doc=lunasvg::Document::loadFromData(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="96"><defs><linearGradient id="g"><stop stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient></defs><path d="M8 64 C20 32 64 32 84 64" stroke="green" fill="none" stroke-width="4"/><rect x="100" y="10" width="12" height="12" fill="url(#g)"/><text id="t" x="12" y="30" font-family="Fixture" font-size="20" fill="red">AB</text></svg>)svg");
 REQUIRE(doc);auto b=doc->renderToBitmap();REQUIRE(!b.isNull());
 REQUIRE(alpha(b,15,20)==255); REQUIRE(alpha(b,28,20)==255);
 REQUIRE(alpha(b,35,20)==255);REQUIRE(alpha(b,5,5)==0);REQUIRE(alpha(b,105,15)==255);
 auto n=covered(b);REQUIRE(n>500);std::printf("LUNASVG_BASELINE_OK font=generated glyphs=2 vector=curve gradient=1 pixels=%u\n",n);
}
