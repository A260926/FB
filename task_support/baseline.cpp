#include <btfparse/ibtf.h>
#include <btfparse/ibtfheadergenerator.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char**argv){
 if(argc!=2)return 2;
 auto r=btfparse::IBTF::createFromPathList({"task_support/samples/base.btf","task_support/samples/left.btf"});
 if(r.failed())throw std::runtime_error(r.takeError().toString());
 auto b=r.takeValue();auto n=b->getType(4);if(!n)throw std::runtime_error("ID4 missing");
 auto p=std::get_if<btfparse::StructBTFType>(&*n);
 if(!p || p->opt_name!="node" || p->member_list.size()!=2 || p->member_list[0].type!=3 || p->member_list[1].type!=2 || p->member_list[1].opt_name!="ghost_name")throw std::runtime_error("split production shape");
 std::string header;auto g=btfparse::IBTFHeaderGenerator::create();if(!g->generate(header,b) || header.empty())throw std::runtime_error("header generation");
 std::ofstream f(argv[1]);f<<header;f.close();if(!f)throw std::runtime_error("header write");
 std::cout<<"LINEAR_SPLIT count="<<b->count()<<" node_size="<<p->size<<" inherited_member="<<p->member_list[1].opt_name.value()<<"\n";
 std::cout<<"HEADER_BYTES="<<header.size()<<"\nBASELINE_OK\n";
}
