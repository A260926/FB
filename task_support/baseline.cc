#include "fixture.h"
#include "wabt/error-formatter.h"
#include "wabt/expr-visitor.h"
#include <cstdio>
struct Counter : wabt::ExprVisitor::DelegateNop {
  size_t direct=0, indirect=0, refs=0;
  wabt::Result OnCallExpr(wabt::CallExpr*) override {++direct;return wabt::Result::Ok;}
  wabt::Result OnCallRefExpr(wabt::CallRefExpr*) override {++indirect;return wabt::Result::Ok;}
  wabt::Result OnRefFuncExpr(wabt::RefFuncExpr*) override {++refs;return wabt::Result::Ok;}
};
int main() {
  auto text=fixture::Load("task_support/reference-inputs.wat");
  wabt::Errors errors;wabt::Module module;std::vector<uint8_t> bytes;
  if (text.empty() || wabt::Failed(fixture::RoundTrip(text,&module,&errors,&bytes))) {
    std::fprintf(stderr,"%s",wabt::FormatErrorsToString(errors,wabt::Location::Type::Text).c_str());return 1;
  }
  Counter counter;wabt::ExprVisitor visitor(&counter);
  for(size_t i=module.num_func_imports;i<module.funcs.size();++i)
    if(wabt::Failed(visitor.VisitFunc(module.funcs[i]))) return 2;
  std::printf("BASELINE_VALIDATED funcs=%zu func_imports=%u bytes=%zu\n",module.funcs.size(),module.num_func_imports,bytes.size());
  std::printf("BASELINE_IR direct=%zu call_ref=%zu ref_func=%zu\n",counter.direct,counter.indirect,counter.refs);
  if(module.funcs.size()!=16 || module.num_func_imports!=1 || counter.direct!=8 || counter.indirect!=10 || counter.refs!=9) return 3;
  std::ofstream out(".task-build/reference-inputs.wasm",std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(!out) return 4;
  std::puts("BASELINE_OK static read/write/validation only; no input execution");
  return 0;
}
