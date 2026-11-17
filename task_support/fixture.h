#ifndef REFERENCE_FIXTURE_H
#define REFERENCE_FIXTURE_H
#include "wabt/binary-reader.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/binary-writer.h"
#include "wabt/resolve-names.h"
#include "wabt/validator.h"
#include "wabt/wast-parser.h"
#include <fstream>
#include <iterator>
namespace fixture {
inline wabt::Features Features() {
  wabt::Features f; f.enable_function_references(); return f;
}
inline wabt::Result RoundTrip(const std::string& text, wabt::Module* output,
                             wabt::Errors* errors, std::vector<uint8_t>* bytes) {
  auto features=Features();
  auto lexer=wabt::WastLexer::CreateBufferLexer("synthetic.wat",text.data(),text.size(),errors);
  wabt::WastParseOptions parse(features);
  std::unique_ptr<wabt::Module> module;
  if (wabt::Failed(wabt::ParseWatModule(lexer.get(),&module,errors,&parse))) return wabt::Result::Error;
  if (wabt::Failed(wabt::ResolveNamesModule(module.get(),errors))) return wabt::Result::Error;
  if (wabt::Failed(wabt::ValidateModule(module.get(),errors,wabt::ValidateOptions(features)))) return wabt::Result::Error;
  wabt::MemoryStream stream;
  wabt::WriteBinaryOptions write(features,true,false,false);
  if (wabt::Failed(wabt::WriteBinaryModule(&stream,module.get(),write))) return wabt::Result::Error;
  *bytes=stream.output_buffer().data;
  wabt::ReadBinaryOptions read(features,nullptr,false,true,true);
  if (wabt::Failed(wabt::ReadBinaryIr("synthetic.wasm",bytes->data(),bytes->size(),read,errors,output))) return wabt::Result::Error;
  return wabt::ValidateModule(output,errors,wabt::ValidateOptions(features));
}
inline std::string Load(const char* path) {
  std::ifstream in(path,std::ios::binary);
  return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
}
#endif
