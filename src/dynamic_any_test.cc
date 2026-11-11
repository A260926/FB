// Copyright 2026 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/any.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "port/gtest.h"
#include "src/mutator.h"
#include "src/mutator_test_proto2.pb.h"

namespace {

namespace pb = google::protobuf;
using protobuf_mutator::Mutator;

const char kPayloadUrl[] = "schemas/runtime.Payload";
const char kUnknownUrl[] = "schemas/runtime.Missing";

// Builds a complete runtime schema in an independent DescriptorPool. Both
// google.protobuf.Any and runtime.Payload / runtime.Container are described by
// hand written FileDescriptorProto values; nothing is taken from the generated
// message factory.
class TestSchema {
 public:
  pb::DescriptorPool pool;
  pb::DynamicMessageFactory factory;
  const pb::Descriptor* any_desc = nullptr;
  const pb::Descriptor* payload_desc = nullptr;
  const pb::Descriptor* container_desc = nullptr;

  explicit TestSchema(bool value_is_string = false) : factory(&pool) {
    BuildAnyFile();
    BuildPayloadFile(value_is_string);
    BuildContainerFile();
    any_desc = pool.FindMessageTypeByName("google.protobuf.Any");
    payload_desc = pool.FindMessageTypeByName("runtime.Payload");
    container_desc = pool.FindMessageTypeByName("runtime.Container");
    if (!any_desc || !payload_desc || !container_desc) std::abort();
  }

 private:
  void BuildAnyFile() {
    pb::FileDescriptorProto file;
    file.set_name("google/protobuf/any.proto");
    file.set_package("google.protobuf");
    file.set_syntax("proto3");
    auto* any = file.add_message_type();
    any->set_name("Any");
    auto* type_url = any->add_field();
    type_url->set_name("type_url");
    type_url->set_number(1);
    type_url->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    type_url->set_type(pb::FieldDescriptorProto::TYPE_STRING);
    auto* value = any->add_field();
    value->set_name("value");
    value->set_number(2);
    value->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    value->set_type(pb::FieldDescriptorProto::TYPE_BYTES);
    BuildFile(&file);
  }

  void BuildPayloadFile(bool value_is_string) {
    pb::FileDescriptorProto file;
    file.set_name("runtime_payload.proto");
    file.set_package("runtime");
    file.set_syntax("proto2");
    file.add_dependency("google/protobuf/any.proto");
    auto* payload = file.add_message_type();
    payload->set_name("Payload");
    auto* id = payload->add_field();
    id->set_name("id");
    id->set_number(1);
    id->set_label(pb::FieldDescriptorProto::LABEL_REQUIRED);
    id->set_type(pb::FieldDescriptorProto::TYPE_INT32);
    auto* value = payload->add_field();
    value->set_name("value");
    value->set_number(2);
    value->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    value->set_type(value_is_string ? pb::FieldDescriptorProto::TYPE_STRING
                                    : pb::FieldDescriptorProto::TYPE_INT32);
    auto* text = payload->add_field();
    text->set_name("text");
    text->set_number(3);
    text->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    text->set_type(pb::FieldDescriptorProto::TYPE_STRING);
    auto* inner = payload->add_field();
    inner->set_name("inner");
    inner->set_number(4);
    inner->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    inner->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    inner->set_type_name(".google.protobuf.Any");
    BuildFile(&file);
  }

  void BuildContainerFile() {
    pb::FileDescriptorProto file;
    file.set_name("runtime_container.proto");
    file.set_package("runtime");
    file.set_syntax("proto3");
    file.add_dependency("google/protobuf/any.proto");
    file.add_dependency("runtime_payload.proto");
    auto* container = file.add_message_type();
    container->set_name("Container");

    auto* single = container->add_field();
    single->set_name("single");
    single->set_number(1);
    single->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    single->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    single->set_type_name(".google.protobuf.Any");

    auto* many = container->add_field();
    many->set_name("many");
    many->set_number(2);
    many->set_label(pb::FieldDescriptorProto::LABEL_REPEATED);
    many->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    many->set_type_name(".google.protobuf.Any");

    auto* payload = container->add_field();
    payload->set_name("payload");
    payload->set_number(3);
    payload->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    payload->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    payload->set_type_name(".runtime.Payload");

    auto* map_entry = container->add_nested_type();
    map_entry->set_name("AnyMapEntry");
    map_entry->mutable_options()->set_map_entry(true);
    auto* key = map_entry->add_field();
    key->set_name("key");
    key->set_number(1);
    key->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    key->set_type(pb::FieldDescriptorProto::TYPE_STRING);
    auto* map_value = map_entry->add_field();
    map_value->set_name("value");
    map_value->set_number(2);
    map_value->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    map_value->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    map_value->set_type_name(".google.protobuf.Any");

    auto* map_any = container->add_field();
    map_any->set_name("map_any");
    map_any->set_number(4);
    map_any->set_label(pb::FieldDescriptorProto::LABEL_REPEATED);
    map_any->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    map_any->set_type_name(".runtime.Container.AnyMapEntry");

    auto* oneof = container->add_oneof_decl();
    oneof->set_name("choice");
    auto* any_choice = container->add_field();
    any_choice->set_name("any_choice");
    any_choice->set_number(5);
    any_choice->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    any_choice->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
    any_choice->set_type_name(".google.protobuf.Any");
    any_choice->set_oneof_index(0);
    auto* num_choice = container->add_field();
    num_choice->set_name("num_choice");
    num_choice->set_number(6);
    num_choice->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
    num_choice->set_type(pb::FieldDescriptorProto::TYPE_INT32);
    num_choice->set_oneof_index(0);

    BuildFile(&file);
  }

  void BuildFile(pb::FileDescriptorProto* file) {
    if (pool.BuildFile(*file) == nullptr) std::abort();
  }
};

const pb::FieldDescriptor* Field(const pb::Descriptor* descriptor, int number) {
  const pb::FieldDescriptor* field = descriptor->FindFieldByNumber(number);
  if (field == nullptr) std::abort();
  return field;
}

std::unique_ptr<pb::Message> NewMessage(TestSchema& schema,
                                        const pb::Descriptor* descriptor) {
  return std::unique_ptr<pb::Message>(
      schema.factory.GetPrototype(descriptor)->New());
}

std::string TypeNameFromUrl(const std::string& type_url) {
  const size_t slash = type_url.rfind('/');
  if (slash == std::string::npos) return std::string();
  return type_url.substr(slash + 1);
}

std::string SerializePartial(const pb::Message& message) {
  std::string result;
  message.SerializePartialToString(&result);
  return result;
}

void SetStringField(pb::Message* message, int number, const std::string& value) {
  const pb::Reflection* reflection = message->GetReflection();
  reflection->SetString(message, Field(message->GetDescriptor(), number),
                        value);
}

std::string GetStringField(const pb::Message& message, int number) {
  return message.GetReflection()->GetString(
      message, Field(message.GetDescriptor(), number));
}

void SetInt32Field(pb::Message* message, int number, int32_t value) {
  const pb::Reflection* reflection = message->GetReflection();
  reflection->SetInt32(message, Field(message->GetDescriptor(), number), value);
}

int32_t GetInt32Field(const pb::Message& message, int number) {
  return message.GetReflection()->GetInt32(
      message, Field(message.GetDescriptor(), number));
}

bool HasField(const pb::Message& message, int number) {
  return message.GetReflection()->HasField(
      message, Field(message.GetDescriptor(), number));
}

void SetAnyFields(pb::Message* any, const std::string& type_url,
                  const std::string& value) {
  SetStringField(any, 1, type_url);
  SetStringField(any, 2, value);
}

std::string AnyTypeUrl(const pb::Message& any) {
  return GetStringField(any, 1);
}

std::string AnyValue(const pb::Message& any) {
  return GetStringField(any, 2);
}

std::unique_ptr<pb::Message> MakeAny(TestSchema& schema,
                                     const std::string& type_url,
                                     const std::string& value) {
  auto any = NewMessage(schema, schema.any_desc);
  SetAnyFields(any.get(), type_url, value);
  return any;
}

std::unique_ptr<pb::Message> MakePayload(TestSchema& schema, int id,
                                         int value, const std::string& text) {
  auto payload = NewMessage(schema, schema.payload_desc);
  SetInt32Field(payload.get(), 1, id);
  SetInt32Field(payload.get(), 2, value);
  SetStringField(payload.get(), 3, text);
  return payload;
}

Mutator::AnyResolver MakeResolver(TestSchema& schema,
                                  std::vector<std::string>* calls = nullptr) {
  return [&schema, calls](const std::string& type_url)
             -> std::unique_ptr<pb::Message> {
    if (calls != nullptr) calls->push_back(type_url);
    const std::string type_name = TypeNameFromUrl(type_url);
    if (type_name.empty()) return nullptr;
    const pb::Descriptor* descriptor =
        schema.pool.FindMessageTypeByName(type_name);
    if (descriptor == nullptr) return nullptr;
    return NewMessage(schema, descriptor);
  };
}

// Parses |payload| into the container's Any field |number|.
void SetContainerAny(pb::Message* container, int number,
                     const std::string& type_url, const std::string& value) {
  pb::Message* any = container->GetReflection()->MutableMessage(
      container, Field(container->GetDescriptor(), number));
  SetAnyFields(any, type_url, value);
}

pb::Message* MutableContainerAny(pb::Message* container, int number) {
  return container->GetReflection()->MutableMessage(
      container, Field(container->GetDescriptor(), number));
}

std::string FieldBytes(int number, const std::string& data) {
  std::string result;
  auto append_varint = [&result](uint64_t value) {
    while (value >= 0x80) {
      result.push_back(static_cast<char>((value & 0x7f) | 0x80));
      value >>= 7;
    }
    result.push_back(static_cast<char>(value));
  };
  append_varint(static_cast<uint64_t>(number) << 3 | 2u);
  append_varint(data.size());
  result.append(data);
  return result;
}

std::unique_ptr<pb::Message> BuildAnyChain(TestSchema& schema,
                                           int payload_layers) {
  auto current = NewMessage(schema, schema.payload_desc);
  SetInt32Field(current.get(), 1, 0);
  for (int layer = payload_layers - 1; layer >= 1; --layer) {
    auto inner_any = MakeAny(schema, kPayloadUrl, SerializePartial(*current));
    auto parent = NewMessage(schema, schema.payload_desc);
    SetInt32Field(parent.get(), 1, 0);
    pb::Message* inner = parent->GetReflection()->MutableMessage(
        parent.get(), Field(parent->GetDescriptor(), 4));
    inner->CopyFrom(*inner_any);
    current = std::move(parent);
  }
  return MakeAny(schema, kPayloadUrl, SerializePartial(*current));
}

TEST(DynamicAny, RuntimeFix) {
  TestSchema schema;
  Mutator mutator;
  mutator.Seed(1);
  std::vector<std::string> resolver_calls;
  mutator.SetAnyResolver(MakeResolver(schema, &resolver_calls));

  auto payload = NewMessage(schema, schema.payload_desc);
  SetInt32Field(payload.get(), 2, 7);
  SetStringField(payload.get(), 3, "input");
  const std::string input_value = SerializePartial(*payload);
  auto envelope = MakeAny(schema, kPayloadUrl, input_value);

  int payload_calls = 0;
  int any_calls = 0;
  mutator.RegisterPostProcessor(
      schema.payload_desc,
      [&](pb::Message* message, unsigned int) {
        ++payload_calls;
        SetInt32Field(message, 2, GetInt32Field(*message, 2) + 1);
      });
  mutator.RegisterPostProcessor(
      schema.any_desc,
      [&](pb::Message*, unsigned int) { ++any_calls; });

  mutator.Fix(envelope.get());

  EXPECT_EQ(1u, resolver_calls.size());
  EXPECT_EQ(kPayloadUrl, resolver_calls.front());
  EXPECT_EQ(1, payload_calls);
  EXPECT_EQ(1, any_calls);
  EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*envelope));
  EXPECT_NE(input_value, AnyValue(*envelope));

  auto fixed = NewMessage(schema, schema.payload_desc);
  ASSERT_TRUE(fixed->ParsePartialFromString(AnyValue(*envelope)));
  EXPECT_TRUE(fixed->IsInitialized());
  // ParsePartial accepted the payload even though the required id was
  // missing, and Fix filled it in.
  EXPECT_TRUE(HasField(*fixed, 1));
  EXPECT_EQ(0, GetInt32Field(*fixed, 1));
  EXPECT_EQ(8, GetInt32Field(*fixed, 2));
  EXPECT_EQ("input", GetStringField(*fixed, 3));

  // The structured mode also recognizes generated Any envelopes.
  {
    Mutator generated_mutator;
    generated_mutator.Seed(2);
    generated_mutator.SetAnyResolver(
        [](const std::string& type_url) -> std::unique_ptr<pb::Message> {
          if (TypeNameFromUrl(type_url) == "protobuf_mutator.Msg.SubMsg")
            return std::unique_ptr<pb::Message>(
                new protobuf_mutator::Msg::SubMsg());
          return nullptr;
        });
    int generated_calls = 0;
    generated_mutator.RegisterPostProcessor(
        protobuf_mutator::Msg::SubMsg::descriptor(),
        [&](pb::Message* message, unsigned int) {
          ++generated_calls;
          const pb::Reflection* reflection = message->GetReflection();
          reflection->SetInt64(
              message, Field(message->GetDescriptor(), 1), 23);
        });
    pb::Any generated_envelope;
    protobuf_mutator::Msg::SubMsg child;
    child.set_optional_int64(7);
    ASSERT_TRUE(generated_envelope.PackFrom(child));
    generated_mutator.Fix(&generated_envelope);
    EXPECT_EQ(1, generated_calls);
    ASSERT_TRUE(generated_envelope.UnpackTo(&child));
    EXPECT_EQ(23, child.optional_int64());
  }
}

TEST(DynamicAny, MutationAndCrossover) {
  TestSchema schema;

  int mutated_payloads = 0;
  for (unsigned int seed = 0; seed < 256; ++seed) {
    Mutator mutator;
    mutator.Seed(seed);
    mutator.SetAnyResolver(MakeResolver(schema));

    auto payload = MakePayload(schema, 1, 7, "seed");
    auto envelope = MakeAny(schema, kPayloadUrl, SerializePartial(*payload));
    mutator.Mutate(envelope.get(), 1024);

    EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*envelope));
    auto mutated = NewMessage(schema, schema.payload_desc);
    ASSERT_TRUE(mutated->ParsePartialFromString(AnyValue(*envelope)));
    EXPECT_TRUE(mutated->IsInitialized());
    if (GetInt32Field(*mutated, 2) != 7 ||
        GetStringField(*mutated, 3) != "seed") {
      ++mutated_payloads;
    }
  }
  EXPECT_GT(mutated_payloads, 0);

  int copied_runs = 0;
  for (unsigned int seed = 0; seed < 256; ++seed) {
    Mutator mutator;
    mutator.Seed(seed);
    mutator.SetAnyResolver(MakeResolver(schema));

    auto donor_payload = MakePayload(schema, 1, 99, "donor");
    auto donor =
        MakeAny(schema, kPayloadUrl, SerializePartial(*donor_payload));
    const std::string donor_bytes = SerializePartial(*donor);

    auto destination_payload = MakePayload(schema, 1, 7, "destination");
    auto destination =
        MakeAny(schema, kPayloadUrl, SerializePartial(*destination_payload));

    mutator.CrossOver(*donor, destination.get(), 1024);

    EXPECT_EQ(donor_bytes, SerializePartial(*donor));
    EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*destination));
    auto crossed = NewMessage(schema, schema.payload_desc);
    ASSERT_TRUE(crossed->ParsePartialFromString(AnyValue(*destination)));
    EXPECT_TRUE(crossed->IsInitialized());
    if (GetInt32Field(*crossed, 2) == 99) ++copied_runs;
  }
  EXPECT_GT(copied_runs, 0);
}

TEST(DynamicAny, OpaqueAndReplacement) {
  TestSchema schema;
  Mutator mutator;
  mutator.Seed(3);
  mutator.SetAnyResolver(MakeResolver(schema));
  int payload_calls = 0;
  int any_calls = 0;
  mutator.RegisterPostProcessor(
      schema.payload_desc,
      [&](pb::Message*, unsigned int) { ++payload_calls; });
  mutator.RegisterPostProcessor(
      schema.any_desc, [&](pb::Message*, unsigned int) { ++any_calls; });

  // Unknown type: exact URL and bytes are preserved and no callbacks run.
  const std::string unknown_value("\x08\x96\x01\xff", 4);
  auto unknown = MakeAny(schema, kUnknownUrl, unknown_value);
  mutator.Fix(unknown.get());
  EXPECT_EQ(kUnknownUrl, AnyTypeUrl(*unknown));
  EXPECT_EQ(unknown_value, AnyValue(*unknown));
  EXPECT_EQ(0, payload_calls);
  EXPECT_EQ(0, any_calls);

  // Malformed bytes for a known type stay opaque as well.
  const std::string malformed_value(1, static_cast<char>(0x80));
  auto malformed = MakeAny(schema, kPayloadUrl, malformed_value);
  mutator.Fix(malformed.get());
  EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*malformed));
  EXPECT_EQ(malformed_value, AnyValue(*malformed));
  EXPECT_EQ(0, payload_calls);
  EXPECT_EQ(0, any_calls);
  mutator.Mutate(malformed.get(), 1024);
  EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*malformed));
  EXPECT_EQ(malformed_value, AnyValue(*malformed));

  // A valid URL whose resolved full name does not match is opaque.
  Mutator mismatched;
  mismatched.Seed(4);
  mismatched.SetAnyResolver(
      [&schema](const std::string&) -> std::unique_ptr<pb::Message> {
        return NewMessage(schema, schema.payload_desc);
      });
  const std::string mismatched_value =
      SerializePartial(*MakePayload(schema, 1, 3, "mismatch"));
  auto mismatch = MakeAny(schema, "schemas/runtime.Other", mismatched_value);
  mismatched.RegisterPostProcessor(
      schema.payload_desc,
      [&](pb::Message*, unsigned int) { ++payload_calls; });
  mismatched.Fix(mismatch.get());
  EXPECT_EQ("schemas/runtime.Other", AnyTypeUrl(*mismatch));
  EXPECT_EQ(mismatched_value, AnyValue(*mismatch));
  EXPECT_EQ(0, payload_calls);

  // URL without a slash is invalid; the resolver must not be consulted.
  std::vector<std::string> invalid_calls;
  Mutator invalid;
  invalid.Seed(5);
  invalid.SetAnyResolver(MakeResolver(schema, &invalid_calls));
  auto invalid_url = MakeAny(schema, "not-a-type-url", "bytes");
  invalid.Fix(invalid_url.get());
  EXPECT_EQ("not-a-type-url", AnyTypeUrl(*invalid_url));
  EXPECT_EQ("bytes", AnyValue(*invalid_url));
  EXPECT_EQ(0u, invalid_calls.size());

  // An opaque envelope can be replaced through a containing message field.
  int replacement_copied = 0;
  std::string donor_envelope_bytes;
  for (unsigned int seed = 0; seed < 256; ++seed) {
    Mutator cross;
    cross.Seed(seed);
    cross.SetAnyResolver(MakeResolver(schema));

    auto donor_payload = MakePayload(schema, 1, 99, "donor");
    auto donor_container = NewMessage(schema, schema.container_desc);
    auto* donor_single = MutableContainerAny(donor_container.get(), 1);
    // Include an unknown field after the two known Any fields.
    donor_envelope_bytes = FieldBytes(1, kPayloadUrl) +
                           FieldBytes(2, SerializePartial(*donor_payload)) +
                           std::string("\x18\x2a");
    ASSERT_TRUE(donor_single->ParsePartialFromString(donor_envelope_bytes));

    auto destination_payload = MakePayload(schema, 1, 7, "destination");
    auto destination_container = NewMessage(schema, schema.container_desc);
    SetContainerAny(destination_container.get(), 1, kPayloadUrl,
                    SerializePartial(*destination_payload));

    cross.CrossOver(*donor_container, destination_container.get(), 1024);

    EXPECT_EQ(donor_envelope_bytes, SerializePartial(*donor_single));
    pb::Message* destination_single =
        MutableContainerAny(destination_container.get(), 1);
    // A whole-envelope copy is visible byte for byte, including the unknown
    // field. Copying only a payload field inside the destination envelope is
    // a different, equally valid mutation and is not counted here.
    if (SerializePartial(*destination_single) != donor_envelope_bytes) continue;
    ++replacement_copied;
    auto copied_payload = NewMessage(schema, schema.payload_desc);
    ASSERT_TRUE(
        copied_payload->ParsePartialFromString(AnyValue(*destination_single)));
    EXPECT_EQ(99, GetInt32Field(*copied_payload, 2));
    EXPECT_EQ("donor", GetStringField(*copied_payload, 3));
    EXPECT_EQ(kPayloadUrl, AnyTypeUrl(*destination_single));
  }
  EXPECT_GT(replacement_copied, 0);

  // Any values inside ordinary fields, repeated fields, map values and oneofs
  // follow the same rules. The container is parsed from wire bytes so that the
  // map entry representation is produced by protobuf itself.
  auto envelope_bytes = [](const std::string& type_url,
                           const std::string& value) {
    return FieldBytes(1, type_url) + FieldBytes(2, value);
  };
  std::string container_wire;
  container_wire += FieldBytes(
      1, envelope_bytes(kPayloadUrl,
                        SerializePartial(*MakePayload(schema, 1, 1, "single"))));
  container_wire += FieldBytes(
      2, envelope_bytes(kPayloadUrl,
                        SerializePartial(*MakePayload(schema, 1, 2, "many"))));
  container_wire += FieldBytes(2, envelope_bytes(kUnknownUrl, "opaque-repeated"));
  container_wire += FieldBytes(
      4, FieldBytes(1, "key") +
             FieldBytes(2, envelope_bytes(
                              kPayloadUrl,
                              SerializePartial(*MakePayload(schema, 1, 3,
                                                            "map")))));
  container_wire += FieldBytes(
      5, envelope_bytes(kPayloadUrl,
                        SerializePartial(*MakePayload(schema, 1, 4, "oneof"))));

  auto container = NewMessage(schema, schema.container_desc);
  ASSERT_TRUE(container->ParsePartialFromString(container_wire));
  const pb::Message* many = &container->GetReflection()->GetRepeatedMessage(
      *container, Field(container->GetDescriptor(), 2), 1);

  payload_calls = 0;
  any_calls = 0;
  mutator.Fix(container.get());
  // single, many[0], map and oneof are structured; many[1] is opaque.
  EXPECT_EQ(4, payload_calls);
  EXPECT_EQ(4, any_calls);
  EXPECT_EQ(kUnknownUrl, AnyTypeUrl(*many));
  EXPECT_EQ("opaque-repeated", AnyValue(*many));
}

TEST(DynamicAny, CallbackOrderAndIsolation) {
  TestSchema schema;
  const std::string url = kPayloadUrl;

  auto inner_payload = MakePayload(schema, 1, 5, "inner");
  auto inner_any = MakeAny(schema, url, SerializePartial(*inner_payload));
  auto outer_payload = MakePayload(schema, 1, 6, "outer");
  pb::Message* outer_inner = outer_payload->GetReflection()->MutableMessage(
      outer_payload.get(), Field(outer_payload->GetDescriptor(), 4));
  outer_inner->CopyFrom(*inner_any);
  auto outer_any = MakeAny(schema, url, SerializePartial(*outer_payload));

  Mutator mutator;
  mutator.Seed(6);
  std::vector<std::string> resolver_calls;
  mutator.SetAnyResolver(MakeResolver(schema, &resolver_calls));
  std::vector<std::string> order;
  mutator.RegisterPostProcessor(
      schema.payload_desc, [&](pb::Message* message, unsigned int) {
        order.push_back("payload:" + GetStringField(*message, 3));
      });
  mutator.RegisterPostProcessor(
      schema.any_desc,
      [&](pb::Message*, unsigned int) { order.push_back("any"); });

  mutator.Fix(outer_any.get());
  EXPECT_EQ(2u, resolver_calls.size());
  EXPECT_THAT(order,
              ::testing::ElementsAre("payload:inner", "any", "payload:outer",
                                     "any"));

  // The payload bytes were written back before the Any callback ran.
  auto fixed_outer = NewMessage(schema, schema.payload_desc);
  ASSERT_TRUE(fixed_outer->ParsePartialFromString(AnyValue(*outer_any)));
  EXPECT_EQ(6, GetInt32Field(*fixed_outer, 2));
  const pb::Message* fixed_inner =
      &fixed_outer->GetReflection()->GetMessage(
          *fixed_outer, Field(fixed_outer->GetDescriptor(), 4));
  auto fixed_inner_payload = NewMessage(schema, schema.payload_desc);
  ASSERT_TRUE(
      fixed_inner_payload->ParsePartialFromString(AnyValue(*fixed_inner)));
  EXPECT_EQ(5, GetInt32Field(*fixed_inner_payload, 2));

  // A callback that rewrites the envelope is final: there is no old-cache
  // overwrite and no expansion of the newly written bytes.
  auto rewritten_payload = MakePayload(schema, 1, 1, "rewritten");
  auto rewritten =
      MakeAny(schema, url, SerializePartial(*rewritten_payload));
  std::vector<std::string> rewrite_calls;
  Mutator rewriter;
  rewriter.Seed(7);
  rewriter.SetAnyResolver(MakeResolver(schema, &rewrite_calls));
  int rewrite_any_calls = 0;
  rewriter.RegisterPostProcessor(
      schema.any_desc, [&](pb::Message* message, unsigned int) {
        ++rewrite_any_calls;
        SetStringField(message, 2, "callback-final");
      });
  rewriter.Fix(rewritten.get());
  EXPECT_EQ("callback-final", AnyValue(*rewritten));
  EXPECT_EQ(url, AnyTypeUrl(*rewritten));
  EXPECT_EQ(1, rewrite_any_calls);
  EXPECT_EQ(1u, rewrite_calls.size());

  // Opaque envelopes do not run their own or their payload callbacks.
  auto opaque = MakeAny(schema, kUnknownUrl, "opaque");
  int opaque_payload_calls = 0;
  int opaque_any_calls = 0;
  Mutator opaque_mutator;
  opaque_mutator.Seed(8);
  opaque_mutator.SetAnyResolver(MakeResolver(schema));
  opaque_mutator.RegisterPostProcessor(
      schema.payload_desc,
      [&](pb::Message*, unsigned int) { ++opaque_payload_calls; });
  opaque_mutator.RegisterPostProcessor(
      schema.any_desc,
      [&](pb::Message*, unsigned int) { ++opaque_any_calls; });
  opaque_mutator.Fix(opaque.get());
  EXPECT_EQ("opaque", AnyValue(*opaque));
  EXPECT_EQ(0, opaque_payload_calls);
  EXPECT_EQ(0, opaque_any_calls);

  // Two Mutators with different pools defining the same full name do not
  // interfere with each other.
  TestSchema schema_a(false);
  TestSchema schema_b(true);
  Mutator mutator_a;
  Mutator mutator_b;
  mutator_a.Seed(9);
  mutator_b.Seed(10);
  mutator_a.SetAnyResolver(MakeResolver(schema_a));
  mutator_b.SetAnyResolver(MakeResolver(schema_b));

  auto payload_a = MakePayload(schema_a, 1, 7, "a");
  auto envelope_a = MakeAny(schema_a, url, SerializePartial(*payload_a));
  mutator_a.RegisterPostProcessor(
      schema_a.payload_desc, [&](pb::Message* message, unsigned int) {
        SetInt32Field(message, 2, GetInt32Field(*message, 2) + 1);
      });
  mutator_a.Fix(envelope_a.get());

  auto payload_b = NewMessage(schema_b, schema_b.payload_desc);
  SetInt32Field(payload_b.get(), 1, 1);
  SetStringField(payload_b.get(), 2, "start");
  auto envelope_b = MakeAny(schema_b, url, SerializePartial(*payload_b));
  mutator_b.RegisterPostProcessor(
      schema_b.payload_desc, [&](pb::Message* message, unsigned int) {
        SetStringField(message, 2, "b");
      });
  mutator_b.Fix(envelope_b.get());

  auto parsed_a = NewMessage(schema_a, schema_a.payload_desc);
  ASSERT_TRUE(parsed_a->ParsePartialFromString(AnyValue(*envelope_a)));
  EXPECT_EQ(8, GetInt32Field(*parsed_a, 2));
  auto parsed_b = NewMessage(schema_b, schema_b.payload_desc);
  ASSERT_TRUE(parsed_b->ParsePartialFromString(AnyValue(*envelope_b)));
  EXPECT_EQ("b", GetStringField(*parsed_b, 2));

  // Reusing one Mutator for successive inputs starts from fresh expansion
  // state each time.
  int sequential_calls = 0;
  Mutator sequential;
  sequential.Seed(11);
  sequential.SetAnyResolver(MakeResolver(schema_a));
  sequential.RegisterPostProcessor(
      schema_a.payload_desc, [&](pb::Message* message, unsigned int) {
        ++sequential_calls;
        SetInt32Field(message, 2, GetInt32Field(*message, 2) + 1);
      });
  auto first_payload = MakePayload(schema_a, 1, 20, "first");
  auto first = MakeAny(schema_a, url, SerializePartial(*first_payload));
  sequential.Fix(first.get());
  auto first_fixed = NewMessage(schema_a, schema_a.payload_desc);
  ASSERT_TRUE(first_fixed->ParsePartialFromString(AnyValue(*first)));
  EXPECT_EQ(21, GetInt32Field(*first_fixed, 2));

  auto second_payload = MakePayload(schema_a, 1, 40, "second");
  auto second = MakeAny(schema_a, url, SerializePartial(*second_payload));
  sequential.Fix(second.get());
  auto second_fixed = NewMessage(schema_a, schema_a.payload_desc);
  ASSERT_TRUE(second_fixed->ParsePartialFromString(AnyValue(*second)));
  EXPECT_EQ(41, GetInt32Field(*second_fixed, 2));
  EXPECT_EQ(2, sequential_calls);

  // The shared 200 level budget stops expansion, sampling and callbacks at the
  // same boundary while preserving the unreachable branch byte for byte.
  const int kPayloadLayers = 101;
  auto chain = BuildAnyChain(schema, kPayloadLayers);
  std::vector<std::string> depth_resolver_calls;
  Mutator depth_mutator;
  depth_mutator.Seed(12);
  depth_mutator.SetAnyResolver(
      MakeResolver(schema, &depth_resolver_calls));
  int depth_payload_calls = 0;
  int depth_any_calls = 0;
  depth_mutator.RegisterPostProcessor(
      schema.payload_desc,
      [&](pb::Message*, unsigned int) { ++depth_payload_calls; });
  depth_mutator.RegisterPostProcessor(
      schema.any_desc,
      [&](pb::Message*, unsigned int) { ++depth_any_calls; });

  const std::string chain_before = SerializePartial(*chain);
  depth_mutator.Fix(chain.get());

  // Layers 1..100 are expanded. The envelope at depth 200 would require
  // payload depth 201, so it and its bytes stay opaque.
  EXPECT_EQ(100, depth_payload_calls);
  EXPECT_EQ(100, depth_any_calls);
  EXPECT_EQ(100u, depth_resolver_calls.size());
  EXPECT_EQ(chain_before, SerializePartial(*chain));
}

}  // namespace
