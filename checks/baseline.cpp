#include "pugixml.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
int main(){pugi::xml_document a,b;
assert(a.load_string("<r xmlns:i='http://www.w3.org/2001/XInclude'><i:include href='parts/a.xml'/></r>"));
assert(b.load_string("<part xml:base='more/'><value>7</value></part>"));
a.child("r").append_copy(b.document_element());
b.reset(); assert(a.child("r").child("part").child("value").text().as_int()==7);
assert(a.child("r").child("i:include"));
assert(a.child("r").select_nodes("*").size()==2);
std::cout<<"PUGI_BASELINE_COPY_AND_LITERAL_INCLUDE_OK\n";}
