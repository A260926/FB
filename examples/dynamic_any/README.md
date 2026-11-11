# Structured `google.protobuf.Any` mutation

Some fuzz targets build their schema at runtime from a `FileDescriptorProto`
(or a descriptor set) in an independent `DescriptorPool` and therefore have no
generated C++ classes. `protobuf_mutator::Mutator` normally treats every
`google.protobuf.Any` as two opaque fields, `type_url` and `value`. The optional
structured Any mode lets such runtime schemas participate in `Fix`, `Mutate`
and `CrossOver`.

## Enabling the mode

The public API lives in `src/mutator.h`:

```cpp
using AnyResolver =
    std::function<std::unique_ptr<google::protobuf::Message>(
        const std::string& type_url)>;

void SetAnyResolver(AnyResolver resolver);
```

The resolver receives the complete `type_url` (for example
`"type.googleapis.com/runtime.Payload"`). For a type it handles, it must return
an **empty** message created from the caller's descriptor and message factory.
Returning `nullptr` means "I do not handle this type". Passing an empty
`std::function` restores the legacy behavior.

```cpp
#include "src/mutator.h"

std::unique_ptr<google::protobuf::Message> ResolveAny(
    const std::string& type_url,
    google::protobuf::DescriptorPool* pool,
    google::protobuf::DynamicMessageFactory* factory) {
  std::string type_name = type_url.substr(type_url.rfind('/') + 1);
  const google::protobuf::Descriptor* descriptor =
      pool->FindMessageTypeByName(type_name);
  if (descriptor == nullptr) return nullptr;
  return std::unique_ptr<google::protobuf::Message>(
      factory->GetPrototype(descriptor)->New());
}

google::protobuf::DescriptorPool pool;
google::protobuf::DynamicMessageFactory factory(&pool);
// ... pool.BuildFile(...) for google/protobuf/any.proto and the runtime schema.

protobuf_mutator::Mutator mutator;
mutator.Seed(123);
mutator.SetAnyResolver([&](const std::string& type_url) {
  return ResolveAny(type_url, &pool, &factory);
});

mutator.Fix(message.get());       // fill required fields, run callbacks
mutator.Mutate(message.get(), 4096);
mutator.CrossOver(donor, message.get(), 4096);
```

The resolver and the pool/factory it uses must outlive every call that can
invoke the resolver. The mode is configured per `Mutator` instance; the global
generated pool is never modified, and two mutators backed by different pools do
not share payload types even when the full names are identical.

Both generated-class `google.protobuf.Any` messages and runtime
`DynamicMessage` copies of `Any` are recognized. Runtime messages are handled
with reflection and are never cast to the generated class.

## What is structured?

An `Any` envelope is structured only when both conditions hold:

1. The resolver returns a message whose full name equals the non-empty type
   name after the last `/` in `type_url`.
2. `value` parses successfully with `ParsePartialFromString`.

A missing proto2 required field does not count as malformed input;
`ParsePartialFromString` accepts it and `Fix` fills the field later.

If the type is unknown, the URL is invalid, the resolved full name does not
match, or `value` fails to parse, the envelope is **opaque**. Its `type_url`,
`value` and unknown fields are preserved byte for byte. Any partially parsed
message is discarded and never written back.

## Mutation rules

A structured `Any` is managed as one envelope:

- The parsed payload takes part in the normal field sampling, insertion,
  deletion, copying and cloning machinery.
- `type_url` and `value` are not mutated as independent string fields.
- When only the payload changes, the original URL and any unknown fields on the
  envelope are kept.
- A normal field containing the envelope may still be deleted, copied, cloned
  or used to switch a `oneof`. `CrossOver` may copy a complete envelope from a
  source input; source inputs are never modified.
- Envelopes newly copied into the destination are expanded and post-processed
  in the same round. A newly copied envelope replaces the target's old cached
  payload rather than being overwritten by it, while payload mutations made in
  place are retained.
- Opaque envelopes can travel with their parent object, but their URL and bytes
  are never separated. They do not take part in internal sampling and neither
  their own post-processor nor any payload post-processor runs.

The same rules apply when an `Any` is nested inside another `Any`, when it
appears in an ordinary message field, a repeated field, a map value or a
`oneof`.

## Depth and callbacks

Expansion, sampling and post-processing share a budget of 200 levels. The root
message is at depth 0; crossing an ordinary message field or an `Any` payload
adds one. When the budget would be exceeded, that branch becomes an opaque
terminal: its original bytes are preserved and its resolver, sampling and
post-processors are not entered.

Post-processing runs from the inside out. Every expanded payload reachable in
the final message gets its registered callback once. Its bytes are written back
to the envelope and then the `Any` callback runs, followed by ordinary parent
messages. Nodes that were only `CrossOver` sources, or that were deleted, get
no callback. If a callback rewrites the envelope, that rewrite is final: the
old payload is not restored and content created by the callback is not expanded
recursively. Expansion state does not survive between calls to the same
`Mutator`.

`max_size_hint` keeps its usual approximate meaning; structured payloads are
not required to obey it as a strict byte limit.
