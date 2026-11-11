// Reproduce the current Any handling through the public Mutator API.
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include "google/protobuf/dynamic_message.h"
#include "src/mutator.h"
#include "src/mutator_test_proto2.pb.h"
namespace pb = google::protobuf;
void require(bool ok, const char* why) {
  if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
int main() {
  protobuf_mutator::Mutator mutator;
  mutator.Seed(17);
  int generated_calls = 0;
  mutator.RegisterPostProcessor(protobuf_mutator::Msg::SubMsg::descriptor(),
    [&](pb::Message* m, unsigned int) {
      ++generated_calls;
      m->GetReflection()->SetInt64(m, m->GetDescriptor()->FindFieldByNumber(1), 23);
    });
  pb::Any normal;
  protobuf_mutator::Msg::SubMsg child;
  child.set_optional_int64(7);
  require(normal.PackFrom(child), "pack generated child");
  mutator.Fix(&normal);
  require(normal.UnpackTo(&child) && child.optional_int64() == 23 && generated_calls == 1,
          "generated Any control must invoke payload callback");
  std::cout << "generated-any callback=1 value=23\n";

  // Copy well-known Any into an independent pool: every object is DynamicMessage.
  pb::DescriptorPool pool;
  pb::FileDescriptorProto any_file;
  pb::Any::descriptor()->file()->CopyTo(&any_file);
  require(pool.BuildFile(any_file) != nullptr, "build independent Any descriptor");
  pb::FileDescriptorProto schema;
  schema.set_name("runtime_payload.proto");
  schema.set_package("runtime");
  schema.set_syntax("proto2");
  schema.add_dependency("google/protobuf/any.proto");
  auto* payload_type = schema.add_message_type();
  payload_type->set_name("Payload");
  auto* id = payload_type->add_field();
  id->set_name("id"); id->set_number(1);
  id->set_label(pb::FieldDescriptorProto::LABEL_REQUIRED);
  id->set_type(pb::FieldDescriptorProto::TYPE_INT32);
  auto* nested = payload_type->add_field();
  nested->set_name("nested"); nested->set_number(2);
  nested->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
  nested->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
  nested->set_type_name(".google.protobuf.Any");
  require(pool.BuildFile(schema) != nullptr, "build runtime payload descriptor");
  pb::DynamicMessageFactory factory(&pool);
  const auto* desc = pool.FindMessageTypeByName("runtime.Payload");
  const auto* any_desc = pool.FindMessageTypeByName("google.protobuf.Any");
  auto make = [&](const pb::Descriptor* d) { return std::unique_ptr<pb::Message>(factory.GetPrototype(d)->New()); };
  auto payload = make(desc);
  payload->GetReflection()->SetInt32(payload.get(), desc->FindFieldByNumber(1), 7);
  std::string body = payload->SerializeAsString();
  auto envelope = make(any_desc);
  auto* reflect = envelope->GetReflection();
  reflect->SetString(envelope.get(), any_desc->FindFieldByNumber(1), "schema.local/runtime.Payload");
  reflect->SetString(envelope.get(), any_desc->FindFieldByNumber(2), body);
  int dynamic_calls = 0;
  mutator.RegisterPostProcessor(desc, [&](pb::Message*, unsigned int) { ++dynamic_calls; });
  mutator.Fix(envelope.get());
  require(dynamic_calls == 0, "starting implementation unexpectedly visits runtime payload");
  require(reflect->GetString(*envelope, any_desc->FindFieldByNumber(2)) == body,
          "baseline runtime payload bytes changed");
  std::cout << "dynamic-any callback=0 payload-id=7\n";

  // ParsePartialFromString fails; current UnpackAny ignores that status.
  pb::Any malformed;
  malformed.set_type_url("type.googleapis.com/protobuf_mutator.Msg.SubMsg");
  malformed.set_value(std::string(1, static_cast<char>(0x80)));
  const auto before = malformed.value();
  generated_calls = 0;
  mutator.Fix(&malformed);
  require(generated_calls == 1 && malformed.value() != before,
          "starting malformed Any no longer invokes callback and rewrites bytes");
  require(malformed.UnpackTo(&child) && child.optional_int64() == 23,
          "malformed payload rewrite control");
  std::cout << "malformed-any input=80 callback=1 rewritten-value=23\n";

  // Exercise actual mutator and crossover, not just Fix.
  int corrupt_envelopes = 0;
  for (unsigned int seed = 0; seed < 128; ++seed) {
    protobuf_mutator::Mutator local;
    local.Seed(seed);
    pb::Any value;
    child.set_optional_int64(7);
    value.PackFrom(child);
    local.Mutate(&value, 1024);
    if (!value.Is<protobuf_mutator::Msg::SubMsg>()) ++corrupt_envelopes;
  }
  require(corrupt_envelopes > 0, "raw Any fields no longer mutate in starting tree");
  pb::Any donor, destination;
  child.set_optional_int64(99); donor.PackFrom(child);
  child.set_optional_int64(7); destination.PackFrom(child);
  std::string donor_before = donor.SerializeAsString();
  mutator.Seed(29);
  mutator.CrossOver(donor, &destination, 1024);
  require(donor.SerializeAsString() == donor_before, "crossover modified source");
  std::cout << "raw-envelope-corruptions=" << corrupt_envelopes << "/128\n";
  std::cout << "raw-envelope-corruption-observed=yes crossover-source-unchanged=yes\n";
  std::cout << "BASELINE_OK\n";
}
