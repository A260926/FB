// Contract tests for split BTF views and the union header generator.
//
// The suite is deliberately self-contained: in addition to the shipped
// samples it builds synthetic BTF bytes in memory so it can exercise
// three-level inheritance, same-ID sibling references, handle lifetime and
// module-order independence without any extra tooling.

#include "task_support/check.h"

#include <btfparse/ibtf.h>
#include <btfparse/ibtfheadergenerator.h>
#include <btfparse/ibtfinspector.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace btfparse;

namespace {

// ---------------------------------------------------------------------------
// Synthetic BTF writer
// ---------------------------------------------------------------------------

using Bytes = std::vector<std::uint8_t>;

void putU32(Bytes &bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
}

std::uint32_t addString(Bytes &string_section, const std::string &text) {
  auto offset = static_cast<std::uint32_t>(string_section.size());
  string_section.insert(string_section.end(), text.begin(), text.end());
  string_section.push_back(0);
  return offset;
}

void putType(Bytes &types, std::uint32_t name_off, std::uint32_t kind,
             std::uint32_t vlen, std::uint32_t size_or_type) {
  putU32(types, name_off);
  putU32(types, (kind << 24) | vlen);
  putU32(types, size_or_type);
}

// Struct/union types that carry bitfield members set the kind_flag bit.
void putFlaggedType(Bytes &types, std::uint32_t name_off, std::uint32_t kind,
                    std::uint32_t vlen, std::uint32_t size_or_type) {
  putU32(types, name_off);
  putU32(types, 0x80000000U | (kind << 24) | vlen);
  putU32(types, size_or_type);
}

// A member record: name offset, type, bit offset (or bitfield encoding).
void putMember(Bytes &types, std::uint32_t name_off, std::uint32_t type,
               std::uint32_t offset) {
  putU32(types, name_off);
  putU32(types, type);
  putU32(types, offset);
}

std::filesystem::path scratchDirectory() {
  auto directory = std::filesystem::temp_directory_path() / "btfparse_split_tests";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return directory;
}

std::filesystem::path writeBTF(const std::string &name, const Bytes &types,
                               const Bytes &strings) {
  Bytes file{0x9f, 0xeb, 1, 0};
  putU32(file, 24);                 // hdr_len
  putU32(file, 0);                  // type_off
  putU32(file, static_cast<std::uint32_t>(types.size()));
  putU32(file, static_cast<std::uint32_t>(types.size()));  // str_off
  putU32(file, static_cast<std::uint32_t>(strings.size()));

  file.insert(file.end(), types.begin(), types.end());
  file.insert(file.end(), strings.begin(), strings.end());

  auto path = scratchDirectory() / name;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(file.data()),
               static_cast<std::streamsize>(file.size()));
  output.close();

  return path;
}

// A split child's string offsets are global: they continue the ancestor
// table. Holds the child's own string bytes and adds `base` to every offset it
// hands out, while the file itself only stores the new bytes.
struct SplitStringTable final {
  Bytes bytes{0};
  std::uint32_t base{0};

  std::uint32_t add(const std::string &text) {
    auto local = static_cast<std::uint32_t>(bytes.size());
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
    return base + local;
  }
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

struct Samples final {
  std::filesystem::path base;
  std::filesystem::path left;
  std::filesystem::path right;

  Samples()
      : base("task_support/samples/base.btf"),
        left("task_support/samples/left.btf"),
        right("task_support/samples/right.btf") {}
};

const Samples &samples() {
  static const Samples value;
  return value;
}

IBTF::Ptr load(const std::filesystem::path &path) {
  auto result = IBTF::createFromPath(path);
  if (result.failed()) {
    std::fprintf(stderr, "failed to load %s: %s\n", path.c_str(),
                 result.takeError().toString().c_str());
    std::abort();
  }
  return result.takeValue();
}

IBTF::Ptr split(const IBTF &base, const std::filesystem::path &path) {
  auto result = IBTF::createSplitFromPath(base, path);
  if (result.failed()) {
    std::fprintf(stderr, "failed to split %s: %s\n", path.c_str(),
                 result.takeError().toString().c_str());
    std::abort();
  }
  return result.takeValue();
}

// getType returns the variant by value, so every inspection helper must
// copy out of it rather than return a reference or pointer into a temporary.
StructBTFType asStruct(const IBTF &view, std::uint32_t id) {
  auto type = view.getType(id);
  if (!type.has_value()) {
    std::fprintf(stderr, "missing type %u\n", id);
    std::abort();
  }
  auto pointer = std::get_if<StructBTFType>(&*type);
  if (pointer == nullptr) {
    std::fprintf(stderr, "type %u is not a struct\n", id);
    std::abort();
  }
  return *pointer;
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// A three-level chain: root -> middle -> leaf. Every layer declares a struct
// named `layer` at its own ID 2, and each layer's struct embeds the previous
// layer's struct by value plus a pointer to itself.
struct ChainFixture final {
  std::filesystem::path root_path;
  std::filesystem::path middle_path;
  std::filesystem::path leaf_path;

  std::uint32_t root_string_size{0};
  std::uint32_t middle_string_size{0};
};

ChainFixture buildChainFixture() {
  ChainFixture fixture;

  // Root: ID1 = int, ID2 = struct root_base { int base_value; }
  {
    Bytes strings{0};
    auto int_name = addString(strings, "int");
    auto struct_name = addString(strings, "root_base");
    auto member_name = addString(strings, "base_value");

    Bytes types;
    putType(types, int_name, 1, 0, 4);
    putU32(types, 0x00000020);  // int encoding: 32 bits, unsigned
    putType(types, struct_name, 4, 1, 4);
    putMember(types, member_name, 1, 0);

    fixture.root_path = writeBTF("chain_root.btf", types, strings);
    fixture.root_string_size = static_cast<std::uint32_t>(strings.size());
  }

  // Middle: global ID3 = ptr -> global ID4; ID4 = struct layer { layer *self;
  // root_base parent; }. The middle file's string offsets continue the root
  // table.
  {
    SplitStringTable strings;
    strings.base = fixture.root_string_size;

    auto struct_name = strings.add("layer");
    auto self_name = strings.add("self");
    auto parent_name = strings.add("parent");

    Bytes types;
    putType(types, 0, 2, 0, 4);              // ID3: ptr -> ID4
    putType(types, struct_name, 4, 2, 12);   // ID4: struct layer, size 12
    putMember(types, self_name, 3, 0);       //   self @ 0
    putMember(types, parent_name, 2, 64);    //   parent @ 8 (root struct)

    fixture.middle_path = writeBTF("chain_middle.btf", types, strings.bytes);
    fixture.middle_string_size =
        static_cast<std::uint32_t>(strings.bytes.size());
  }

  // Leaf: global ID5 = ptr -> global ID6; ID6 = struct layer { layer *self;
  // layer parent; int tail; }. Its string offsets continue the ancestor
  // tables.
  {
    SplitStringTable strings;
    strings.base = fixture.root_string_size + fixture.middle_string_size;

    auto struct_name = strings.add("layer");
    auto self_name = strings.add("self");
    auto parent_name = strings.add("parent");
    auto tail_name = strings.add("tail");

    Bytes types;
    putType(types, 0, 2, 0, 6);              // ID5: ptr -> ID6
    putType(types, struct_name, 4, 3, 24);   // ID6: struct layer, size 24
    putMember(types, self_name, 5, 0);       //   self @ 0
    putMember(types, parent_name, 4, 64);    //   parent @ 8 (middle layer)
    putMember(types, tail_name, 1, 192);     //   tail @ 24

    fixture.leaf_path = writeBTF("chain_leaf.btf", types, strings.bytes);
  }

  return fixture;
}

// Two siblings whose ID 4 is each a struct named `node`, but with different
// size and different member references.
struct SiblingFixture final {
  std::filesystem::path base_path;
  std::filesystem::path narrow_path;
  std::filesystem::path wide_path;
};

SiblingFixture buildSiblingFixture() {
  SiblingFixture fixture;

  std::uint32_t base_string_size{0};

  {
    Bytes strings{0};
    auto int_name = addString(strings, "int");
    auto shared_name = addString(strings, "shared");
    auto value_name = addString(strings, "value");

    Bytes types;
    putType(types, int_name, 1, 0, 4);
    putU32(types, 0x00000020);
    putType(types, shared_name, 4, 1, 4);
    putMember(types, value_name, 1, 0);

    fixture.base_path = writeBTF("sibling_base.btf", types, strings);
    base_string_size = static_cast<std::uint32_t>(strings.size());
  }

  auto writeNode = [&](const std::filesystem::path &path,
                       std::uint32_t struct_size) {
    SplitStringTable strings;
    strings.base = base_string_size;

    auto node_name = strings.add("node");
    auto next_name = strings.add("next");
    auto payload_name = strings.add("payload");

    Bytes types;
    putType(types, 0, 2, 0, 4);                    // global ID3: ptr -> ID4
    putType(types, node_name, 4, 2, struct_size);  // global ID4: struct node
    putMember(types, next_name, 3, 0);             //   next @ 0
    putMember(types, payload_name, 2, 64);         //   payload (base shared)

    return writeBTF(path.filename().string(), types, strings.bytes);
  };

  fixture.narrow_path = writeNode(scratchDirectory() / "sibling_narrow.btf", 16);
  fixture.wide_path = writeNode(scratchDirectory() / "sibling_wide.btf", 24);

  return fixture;
}

// A file with one struct containing a bitfield, used to check that the
// inspector marks bitfield layouts as unexplainable rather than guessing.
std::filesystem::path buildBitfieldFixture() {
  Bytes strings{0};
  auto int_name = addString(strings, "int");
  auto flags_name = addString(strings, "flags");
  auto low_name = addString(strings, "low");

  Bytes types;
  putType(types, int_name, 1, 0, 4);
  putU32(types, 0x00000020);
  putFlaggedType(types, 0, 4, 1, 4);       // anonymous struct, size 4
  putMember(types, low_name, 1, 3 << 24);  // bitfield: 3 bits at offset 0

  return writeBTF("bitfield_base.btf", types, strings);
}

// A file with a struct that embeds an incomplete (forward-declared) struct
// by value. Slicing must be refused for that layout.
std::filesystem::path buildIncompleteFixture() {
  Bytes strings{0};
  auto mystery_name = addString(strings, "mystery");
  auto member_name = addString(strings, "body");

  Bytes types;
  putType(types, mystery_name, 7, 0, 0);   // ID1: fwd struct mystery
  putType(types, 0, 4, 1, 4);              // ID2: anonymous struct
  putMember(types, member_name, 1, 0);     //   body @ 0, incomplete type

  return writeBTF("incomplete_base.btf", types, strings);
}

template <typename Type>
std::optional<Type> getAs(const IBTF &view, std::uint32_t id) {
  auto type = view.getType(id);
  if (!type.has_value()) {
    return std::nullopt;
  }

  auto pointer = std::get_if<Type>(&*type);
  if (pointer == nullptr) {
    return std::nullopt;
  }

  return *pointer;
}

} // namespace

// ---------------------------------------------------------------------------
// scopes
// ---------------------------------------------------------------------------

static int scopesSiblingIDsAreDistinct() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  // Ancestors remain visible at their original IDs.
  TASK_CHECK(base->count() == 2);
  TASK_CHECK(left->count() == 4);
  TASK_CHECK(right->count() == 4);

  TASK_CHECK(left->getKind(1) == BTFKind::Int);
  TASK_CHECK(left->getKind(2) == BTFKind::Struct);
  TASK_CHECK(right->getKind(1) == BTFKind::Int);
  TASK_CHECK(right->getKind(2) == BTFKind::Struct);

  // Local numbering starts right after the highest ancestor ID.
  TASK_CHECK(left->getKind(3) == BTFKind::Ptr);
  TASK_CHECK(left->getKind(4) == BTFKind::Struct);
  TASK_CHECK(right->getKind(3) == BTFKind::Ptr);
  TASK_CHECK(right->getKind(4) == BTFKind::Struct);

  // Void is 0 on every view.
  TASK_CHECK(!left->getType(0).has_value());
  TASK_CHECK(!right->getType(0).has_value());

  return 0;
}

static int scopesSiblingsSameIDDifferentType() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  const auto &left_node = asStruct(*left, 4);
  const auto &right_node = asStruct(*right, 4);

  TASK_CHECK(left_node.size == 16);
  TASK_CHECK(right_node.size == 24);

  // Left's ghost_name is the base struct; right's is the base int. Same
  // numeric member type ID (2)? No: left keeps 2 (ancestor), right keeps 1
  // (ancestor). The distinguishing fact is the resolved kind, not the name.
  TASK_CHECK(left_node.member_list[1].type == 2);
  TASK_CHECK(right_node.member_list[1].type == 1);
  TASK_CHECK(left->getKind(left_node.member_list[1].type) == BTFKind::Struct);
  TASK_CHECK(right->getKind(right_node.member_list[1].type) == BTFKind::Int);

  // Recursive pointers resolve to each sibling's own struct.
  TASK_CHECK(left_node.member_list[0].type == 3);
  TASK_CHECK(right_node.member_list[0].type == 3);

  auto left_ptr = getAs<PtrBTFType>(*left, 3);
  auto right_ptr = getAs<PtrBTFType>(*right, 3);
  TASK_CHECK(left_ptr.has_value());
  TASK_CHECK(right_ptr.has_value());
  TASK_CHECK(left_ptr->type == 4);
  TASK_CHECK(right_ptr->type == 4);

  return 0;
}

static int scopesProvenanceRecordsSourceAndDepth() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  auto base_scope = base->getTypeScope(2);
  auto left_ancestor_scope = left->getTypeScope(2);
  auto left_local_scope = left->getTypeScope(4);
  auto right_local_scope = right->getTypeScope(4);

  TASK_CHECK(base_scope.has_value());
  TASK_CHECK(left_ancestor_scope.has_value());
  TASK_CHECK(left_local_scope.has_value());
  TASK_CHECK(right_local_scope.has_value());

  TASK_CHECK(base_scope->depth == 0);
  TASK_CHECK(left_ancestor_scope->depth == 0);
  TASK_CHECK(left_local_scope->depth == 1);
  TASK_CHECK(right_local_scope->depth == 1);

  // Ancestor provenance is shared; sibling provenance is not.
  TASK_CHECK(left_ancestor_scope->source == base_scope->source);
  TASK_CHECK(right->getTypeScope(2)->source == base_scope->source);
  TASK_CHECK(left_local_scope->source != right_local_scope->source);
  TASK_CHECK(left_local_scope->source != base_scope->source);

  // Scope IDs name the type in this view's numbering.
  TASK_CHECK(left_local_scope->id == 4);
  TASK_CHECK(right_local_scope->id == 4);

  return 0;
}

static int scopesStringSectionRetainsAncestorBytes() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);

  // The child must retain the ancestor string table, otherwise ghost_name
  // (used only by the children) could not have been decoded, and a
  // grandchild could not either.
  auto base_strings = base->getStringSection();
  auto left_strings = left->getStringSection();

  TASK_CHECK(!base_strings.empty());
  TASK_CHECK(left_strings.size() >= base_strings.size());
  TASK_CHECK(left_strings.compare(0, base_strings.size(), base_strings) == 0);
  TASK_CHECK(contains(left_strings, "ghost_name"));

  return 0;
}

static int scopesThreeLevelChain() {
  auto fixture = buildChainFixture();

  auto root = load(fixture.root_path);
  auto middle = split(*root, fixture.middle_path);
  auto leaf = split(*middle, fixture.leaf_path);

  // Root: 2 types. Middle adds 2 -> 4. Leaf adds 2 -> 6.
  TASK_CHECK(root->count() == 2);
  TASK_CHECK(middle->count() == 4);
  TASK_CHECK(leaf->count() == 6);

  // Ancestors remain visible all the way down.
  TASK_CHECK(leaf->getKind(2) == BTFKind::Struct);
  TASK_CHECK(leaf->getKind(4) == BTFKind::Struct);

  const auto &middle_layer = asStruct(*middle, 4);
  TASK_CHECK(middle_layer.size == 12);
  TASK_CHECK(middle_layer.member_list[0].type == 3);
  TASK_CHECK(middle_layer.member_list[1].type == 2);

  const auto &leaf_layer = asStruct(*leaf, 6);
  TASK_CHECK(leaf_layer.size == 24);
  TASK_CHECK(leaf_layer.member_list[0].type == 5);  // self pointer
  TASK_CHECK(leaf_layer.member_list[1].type == 4);  // middle layer by value
  TASK_CHECK(leaf_layer.member_list[2].type == 1);  // base int

  TASK_CHECK(leaf->getKind(5) == BTFKind::Ptr);
  TASK_CHECK(leaf->getKind(4) == BTFKind::Struct);

  // Depths increase per layer and sources differ per load.
  TASK_CHECK(leaf->getTypeScope(1)->depth == 0);
  TASK_CHECK(leaf->getTypeScope(4)->depth == 1);
  TASK_CHECK(leaf->getTypeScope(6)->depth == 2);
  TASK_CHECK(leaf->getTypeScope(4)->source == middle->getOriginToken());
  TASK_CHECK(leaf->getTypeScope(6)->source == leaf->getOriginToken());
  TASK_CHECK(root->getOriginToken() != middle->getOriginToken());

  return 0;
}

static int scopesSurviveParentAndFileRelease() {
  auto fixture = buildChainFixture();

  IBTF::Ptr leaf;
  {
    // Load the root, then delete its source file before any split is built.
    // Splitting must use the parsed view, never re-open the ancestor file.
    auto root = load(fixture.root_path);

    std::error_code error;
    std::filesystem::remove(fixture.root_path, error);
    TASK_CHECK(!error);
    TASK_CHECK(!std::filesystem::exists(fixture.root_path));

    auto middle = split(*root, fixture.middle_path);
    TASK_CHECK(middle->count() == 4);

    // The middle file is removed after the middle view is built; the leaf is
    // built from the middle view, then both parent handles go away.
    std::filesystem::remove(fixture.middle_path, error);
    TASK_CHECK(!error);

    leaf = split(*middle, fixture.leaf_path);
    TASK_CHECK(!std::filesystem::exists(fixture.middle_path));

    // root and middle are destroyed at the end of this scope.
  }

  // The leaf still resolves ancestor strings and types with no parent and
  // no source files other than its own.
  TASK_CHECK(leaf->count() == 6);

  const auto &leaf_layer = asStruct(*leaf, 6);
  TASK_CHECK(leaf_layer.size == 24);
  TASK_CHECK(leaf_layer.member_list[1].type == 4);

  const auto &middle_layer = asStruct(*leaf, 4);
  TASK_CHECK(middle_layer.member_list[0].type == 3);
  TASK_CHECK(middle_layer.member_list[1].type == 2);

  TASK_CHECK(leaf->getTypeScope(6)->depth == 2);

  // Ancestor names were decoded before the files disappeared and are still
  // intact.
  TASK_CHECK(middle_layer.opt_name.has_value());
  TASK_CHECK(middle_layer.opt_name.value() == "layer");

  return 0;
}

static int scopesSiblingSameIDDifferentReferences() {
  auto fixture = buildSiblingFixture();

  auto base = load(fixture.base_path);
  auto narrow = split(*base, fixture.narrow_path);
  auto wide = split(*base, fixture.wide_path);

  // Both siblings declare their own ID 4 named `node`, with different sizes.
  const auto &narrow_node = asStruct(*narrow, 4);
  const auto &wide_node = asStruct(*wide, 4);
  TASK_CHECK(narrow_node.size == 16);
  TASK_CHECK(wide_node.size == 24);

  // Both point at the base `shared` for payload, so the reference is the
  // same numeric ancestor ID, but each self-pointer resolves to its own node.
  TASK_CHECK(narrow_node.member_list[1].type == 2);
  TASK_CHECK(wide_node.member_list[1].type == 2);

  TASK_CHECK(narrow->getTypeScope(4)->source != wide->getTypeScope(4)->source);
  TASK_CHECK(narrow->getTypeScope(3)->source == narrow->getOriginToken());
  TASK_CHECK(wide->getTypeScope(3)->source == wide->getOriginToken());

  // The pointer at local ID 3 resolves to the sibling's own ID 4.
  auto narrow_ptr = getAs<PtrBTFType>(*narrow, 3);
  auto wide_ptr = getAs<PtrBTFType>(*wide, 3);
  TASK_CHECK(narrow_ptr.has_value() && narrow_ptr->type == 4);
  TASK_CHECK(wide_ptr.has_value() && wide_ptr->type == 4);

  // Inspect each with its own object bytes: the pointers must each denote
  // the local node type, never the sibling's.
  std::vector<std::uint8_t> narrow_bytes(16, 0x11);
  std::vector<std::uint8_t> wide_bytes(24, 0x22);

  auto narrow_result = inspectSplitType(*narrow, "narrow", 4, narrow_bytes);
  auto wide_result = inspectSplitType(*wide, "wide", 4, wide_bytes);
  TASK_CHECK(!narrow_result.failed());
  TASK_CHECK(!wide_result.failed());

  auto narrow_inspection = narrow_result.takeValue();
  auto wide_inspection = wide_result.takeValue();
  TASK_CHECK(narrow_inspection.size == 16);
  TASK_CHECK(wide_inspection.size == 24);
  TASK_CHECK(narrow_inspection.source == narrow->getTypeScope(4)->source);
  TASK_CHECK(wide_inspection.source == wide->getTypeScope(4)->source);
  TASK_CHECK(narrow_inspection.source != wide_inspection.source);

  return 0;
}

// ---------------------------------------------------------------------------
// declarations
// ---------------------------------------------------------------------------

static int declarationsSharedBaseDeclaredOnce() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  auto generator = IBTFHeaderGenerator::create();
  TASK_CHECK(generator != nullptr);

  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"base", base.get()}, {"left", left.get()}, {"right", right.get()}};
  TASK_CHECK(generator->generateModules(header, modules));
  TASK_CHECK(!header.empty());

  // One base declaration, regardless of how many siblings inherit it.
  std::size_t count{0};
  std::size_t position{0};
  while ((position = header.find("struct base__shared", position)) !=
         std::string::npos) {
    ++count;
    position += 1;
  }

  // "struct base__shared" appears in the declaration and in each member that
  // embeds it (left's ghost_name). Only the declaration line ends in "{".
  std::size_t definition_count{0};
  position = 0;
  while ((position = header.find("struct base__shared {", position)) !=
         std::string::npos) {
    ++definition_count;
    ++position;
  }

  TASK_CHECK(definition_count == 1);
  TASK_CHECK(count >= 1);

  // Each module's own tag is prefixed.
  TASK_CHECK(contains(header, "struct left__node"));
  TASK_CHECK(contains(header, "struct right__node"));

  return 0;
}

static int declarationsSelfReferencesStayLocal() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  auto generator = IBTFHeaderGenerator::create();
  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"base", base.get()}, {"left", left.get()}, {"right", right.get()}};
  TASK_CHECK(generator->generateModules(header, modules));

  TASK_CHECK(contains(header, "struct left__node * next;"));
  TASK_CHECK(contains(header, "struct right__node * next;"));

  // Neither sibling's pointer may resolve into the other's struct.
  TASK_CHECK(!contains(header, "struct left__node * next;\n  /* BTF Type"
                               " #4 */\n  struct right__node"));

  // Left embeds the base struct; right embeds the base int.
  TASK_CHECK(contains(header, "struct base__shared ghost_name;"));
  TASK_CHECK(contains(header, "int ghost_name;"));

  return 0;
}

static int declarationsModuleOrderIndependent() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  auto generator = IBTFHeaderGenerator::create();

  std::string forward_header;
  std::string reverse_header;

  std::vector<std::pair<std::string, const IBTF *>> forward{
      {"base", base.get()}, {"left", left.get()}, {"right", right.get()}};
  std::vector<std::pair<std::string, const IBTF *>> reverse{
      {"right", right.get()}, {"left", left.get()}, {"base", base.get()}};

  TASK_CHECK(generator->generateModules(forward_header, forward));
  TASK_CHECK(generator->generateModules(reverse_header, reverse));

  TASK_CHECK(!forward_header.empty());
  TASK_CHECK(forward_header == reverse_header);

  return 0;
}

static int declarationsIndependentBasesNeverMerge() {
  // Two independent loads of byte-identical base files must stay distinct.
  auto first = load(samples().base);
  auto second = load(samples().base);

  TASK_CHECK(first->getOriginToken() != second->getOriginToken());

  auto generator = IBTFHeaderGenerator::create();
  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"alpha", first.get()}, {"beta", second.get()}};
  TASK_CHECK(generator->generateModules(header, modules));

  // Both bases declare `shared`, under two distinct module prefixes.
  TASK_CHECK(contains(header, "struct alpha__shared"));
  TASK_CHECK(contains(header, "struct beta__shared"));

  std::size_t alpha_definition{0};
  std::size_t position{0};
  while ((position = header.find("struct alpha__shared {", position)) !=
         std::string::npos) {
    ++alpha_definition;
    ++position;
  }

  std::size_t beta_definition{0};
  position = 0;
  while ((position = header.find("struct beta__shared {", position)) !=
         std::string::npos) {
    ++beta_definition;
    ++position;
  }

  TASK_CHECK(alpha_definition == 1);
  TASK_CHECK(beta_definition == 1);

  return 0;
}

static int declarationsRejectDuplicateModuleNames() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);

  auto generator = IBTFHeaderGenerator::create();
  std::string header = "sentinel";

  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"same", base.get()}, {"same", left.get()}};
  TASK_CHECK(!generator->generateModules(header, modules));

  // Failure must not leave a partial header behind.
  TASK_CHECK(header.empty());

  return 0;
}

static int declarationsRejectInvalidModuleNames() {
  auto base = load(samples().base);
  auto generator = IBTFHeaderGenerator::create();

  const char *invalid_names[] = {"", "1base", "base-name", "base name",
                                 "base.name", "base/name"};

  for (const auto *name : invalid_names) {
    std::string header = "sentinel";
    std::vector<std::pair<std::string, const IBTF *>> modules{
        {name, base.get()}};
    TASK_CHECK(!generator->generateModules(header, modules));
    TASK_CHECK(header.empty());
  }

  // Valid C identifiers are accepted, including leading underscores.
  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"_base", base.get()}};
  TASK_CHECK(generator->generateModules(header, modules));
  TASK_CHECK(!header.empty());

  return 0;
}

static int declarationsRejectMissingSourceView() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);

  auto generator = IBTFHeaderGenerator::create();
  std::string header = "sentinel";

  // left inherits types introduced by `base`; omitting the base view leaves
  // those types without a source module and must be rejected.
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"left", left.get()}};
  TASK_CHECK(!generator->generateModules(header, modules));
  TASK_CHECK(header.empty());

  // The root alone is fine.
  std::string root_header;
  std::vector<std::pair<std::string, const IBTF *>> root_modules{
      {"base", base.get()}};
  TASK_CHECK(generator->generateModules(root_header, root_modules));
  TASK_CHECK(!root_header.empty());

  return 0;
}

static int declarationsRejectNullView() {
  auto generator = IBTFHeaderGenerator::create();
  std::string header = "sentinel";

  std::vector<std::pair<std::string, const IBTF *>> modules{{"base", nullptr}};
  TASK_CHECK(!generator->generateModules(header, modules));
  TASK_CHECK(header.empty());

  return 0;
}

static int declarationsEnumMembersAreScoped() {
  // Two modules each declare an enum with a colliding member name.
  Bytes shared_strings{0};
  auto first_enum_name = addString(shared_strings, "mode");
  auto second_enum_name = addString(shared_strings, "mode");
  auto on_name = addString(shared_strings, "MODE_ON");
  auto off_name = addString(shared_strings, "MODE_OFF");

  auto writeEnum = [&](const std::string &file_name) {
    Bytes strings = shared_strings;
    auto enum_name = addString(strings, "mode");
    auto on_member = addString(strings, "MODE_ON");
    auto off_member = addString(strings, "MODE_OFF");

    Bytes types;
    putType(types, enum_name, 6, 2, 4);  // ID1: enum mode, size 4
    putU32(types, on_member);
    putU32(types, 0);
    putU32(types, off_member);
    putU32(types, 1);

    return writeBTF(file_name, types, strings);
  };

  auto first_path = writeEnum("enum_first.btf");
  auto second_path = writeEnum("enum_second.btf");

  auto first = load(first_path);
  auto second = load(second_path);

  auto generator = IBTFHeaderGenerator::create();
  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"one", first.get()}, {"two", second.get()}};
  TASK_CHECK(generator->generateModules(header, modules));

  TASK_CHECK(contains(header, "enum one__mode"));
  TASK_CHECK(contains(header, "enum two__mode"));
  TASK_CHECK(contains(header, "one__mode__MODE_ON"));
  TASK_CHECK(contains(header, "two__mode__MODE_ON"));

  (void)first_enum_name;
  (void)second_enum_name;
  (void)on_name;
  (void)off_name;

  return 0;
}

static int declarationsFundamentalIntNamesUnchanged() {
  auto base = load(samples().base);
  auto generator = IBTFHeaderGenerator::create();

  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"base", base.get()}};
  TASK_CHECK(generator->generateModules(header, modules));

  // Fundamentals keep their spelling and are never prefixed.
  TASK_CHECK(contains(header, "int value;"));
  TASK_CHECK(!contains(header, "base__int"));

  return 0;
}

static int declarationsTypesReferencedByAllKinds() {
  // One module with an array of pointers, a function prototype and a
  // typedef, to confirm every reference field is rewritten, not just struct
  // members.
  Bytes strings{0};
  auto int_name = addString(strings, "int");
  auto node_name = addString(strings, "node");
  auto next_name = addString(strings, "next");
  auto arr_name = addString(strings, "arr");
  auto handler_name = addString(strings, "handler");
  auto node_alias = addString(strings, "node_alias");
  auto node_tag = addString(strings, "node_tag");

  Bytes types;
  putType(types, int_name, 1, 0, 4);       // ID1: int
  putU32(types, 0x00000020);
  putType(types, node_tag, 4, 1, 8);       // ID2: struct node_tag { int next; }
  putMember(types, next_name, 1, 0);
  putType(types, 0, 2, 0, 2);              // ID3: ptr -> ID2
  putType(types, 0, 3, 0, 0);              // ID4: array
  putU32(types, 3);                        //   element type ID3
  putU32(types, 1);                        //   index type ID1
  putU32(types, 2);                        //   2 elements
  putType(types, 0, 13, 1, 1);             // ID5: func proto -> int
  putU32(types, 0);                        //   one param: unnamed
  putU32(types, 3);                        //   param type ID3
  putType(types, node_alias, 8, 0, 2);     // ID6: typedef node_alias -> ID2
  putType(types, 0, 2, 0, 5);              // ID7: ptr -> func proto ID5
  putType(types, arr_name, 4, 1, 16);      // ID8: struct holder { array }
  putMember(types, arr_name, 4, 0);        //   arr[2] @ 0
  putType(types, handler_name, 4, 1, 8);   // ID9: struct fn_holder
  putMember(types, handler_name, 7, 0);    //   handler ptr @ 0

  auto path = writeBTF("allkinds_base.btf", types, strings);
  auto base = load(path);

  auto generator = IBTFHeaderGenerator::create();
  std::string header;
  std::vector<std::pair<std::string, const IBTF *>> modules{
      {"m", base.get()}};
  TASK_CHECK(generator->generateModules(header, modules));

  TASK_CHECK(contains(header, "struct m__node_tag"));
  TASK_CHECK(contains(header, "typedef"));
  TASK_CHECK(contains(header, "m__node_alias"));

  // Both the array element and the function-prototype parameter must point
  // at the renamed node tag, not at the raw record or the unknown name.
  std::size_t occurrences{0};
  std::size_t position{0};
  while ((position = header.find("struct m__node_tag *", position)) !=
         std::string::npos) {
    ++occurrences;
    position += 1;
  }
  TASK_CHECK(occurrences >= 2);

  return 0;
}

// ---------------------------------------------------------------------------
// compatibility
// ---------------------------------------------------------------------------

static int compatibilityLinearInterfaceUnchanged() {
  // The original linear interface still treats its path list as one BTF
  // chain and keeps the baseline shape.
  auto linear = IBTF::createFromPathList({samples().base, samples().left});
  TASK_CHECK(!linear.failed());
  auto view = linear.takeValue();

  TASK_CHECK(view->count() == 4);

  auto node_type = view->getType(4);
  TASK_CHECK(node_type.has_value());

  const auto *node = std::get_if<StructBTFType>(&*node_type);
  TASK_CHECK(node != nullptr);
  TASK_CHECK(node->opt_name.has_value() && node->opt_name.value() == "node");
  TASK_CHECK(node->size == 16);
  TASK_CHECK(node->member_list.size() == 2);
  TASK_CHECK(node->member_list[0].type == 3);
  TASK_CHECK(node->member_list[1].type == 2);
  TASK_CHECK(node->member_list[1].opt_name.has_value() &&
             node->member_list[1].opt_name.value() == "ghost_name");

  return 0;
}

static int compatibilitySingleGenerateUnchanged() {
  auto linear = IBTF::createFromPathList({samples().base, samples().left});
  TASK_CHECK(!linear.failed());
  auto view = linear.takeValue();

  auto generator = IBTFHeaderGenerator::create();
  std::string header;
  TASK_CHECK(generator->generate(header, view));
  TASK_CHECK(!header.empty());

  // The single-view path keeps the historical, unprefixed tag names.
  TASK_CHECK(contains(header, "struct shared"));
  TASK_CHECK(contains(header, "struct node"));
  TASK_CHECK(!contains(header, "left__node"));

  return 0;
}

static int compatibilitySplitFromPathListRoot() {
  // createSplitFromPath works on a linearly-loaded root, not only on a file
  // loaded by createFromPath.
  auto root_result = IBTF::createFromPathList({samples().base});
  TASK_CHECK(!root_result.failed());
  auto root = root_result.takeValue();

  auto left = split(*root, samples().left);
  TASK_CHECK(left->count() == 4);
  TASK_CHECK(left->getKind(4) == BTFKind::Struct);
  TASK_CHECK(asStruct(*left, 4).size == 16);

  return 0;
}

static int compatibilityMissingFileFails() {
  auto base = load(samples().base);
  auto result = IBTF::createSplitFromPath(*base, "task_support/samples/nope.btf");
  TASK_CHECK(result.failed());
  TASK_CHECK(result.takeError().get().code ==
             BTFErrorInformation::Code::FileNotFound);

  return 0;
}

static int compatibilityInvalidSplitReferenceRejected() {
  // A child whose type references an ID that is neither an ancestor nor one
  // of its own must be rejected, not silently tolerated.
  Bytes strings{0};
  auto node_name = addString(strings, "node");
  auto next_name = addString(strings, "next");

  Bytes types;
  putType(types, node_name, 4, 1, 4);  // ID3: struct node
  putMember(types, next_name, 99, 0);  //   dangling type ID 99

  auto path = writeBTF("dangling.btf", types, strings);
  auto base = load(samples().base);

  auto result = IBTF::createSplitFromPath(*base, path);
  TASK_CHECK(result.failed());
  TASK_CHECK(result.takeError().get().code ==
             BTFErrorInformation::Code::InvalidSplitTypeReference);

  return 0;
}

static int compatibilityForensicsDistinguishSiblings() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);
  auto right = split(*base, samples().right);

  std::vector<std::uint8_t> left_bytes(16, 0);
  for (std::size_t i = 0; i < 8; ++i) {
    left_bytes[i] = static_cast<std::uint8_t>(i + 1);
  }

  std::vector<std::uint8_t> right_bytes(24, 0);
  for (std::size_t i = 0; i < 8; ++i) {
    right_bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
  }

  auto left_result = inspectSplitType(*left, "left", 4, left_bytes);
  auto right_result = inspectSplitType(*right, "right", 4, right_bytes);
  TASK_CHECK(!left_result.failed());
  TASK_CHECK(!right_result.failed());

  auto left_inspection = left_result.takeValue();
  auto right_inspection = right_result.takeValue();

  TASK_CHECK(left_inspection.size == 16);
  TASK_CHECK(right_inspection.size == 24);
  TASK_CHECK(left_inspection.explainable);
  TASK_CHECK(right_inspection.explainable);

  // Same requested ID and same local name, distinct provenance and layout.
  TASK_CHECK(left_inspection.requested_id == 4);
  TASK_CHECK(right_inspection.requested_id == 4);
  TASK_CHECK(contains(left_inspection.type_name, "node"));
  TASK_CHECK(contains(right_inspection.type_name, "node"));
  TASK_CHECK(left_inspection.source != right_inspection.source);

  // Each next pointer points at its own struct, reported as a stored value.
  TASK_CHECK(left_inspection.members.size() == 2);
  TASK_CHECK(right_inspection.members.size() == 2);

  TASK_CHECK(left_inspection.members[0].is_pointer);
  TASK_CHECK(left_inspection.members[0].pointer_value == 0x0807060504030201ULL);
  TASK_CHECK(right_inspection.members[0].is_pointer);
  TASK_CHECK(right_inspection.members[0].pointer_value == 0xA7A6A5A4A3A2A1A0ULL);

  // The inherited member keeps ancestor provenance in both views.
  TASK_CHECK(left_inspection.members[1].type_source ==
             base->getOriginToken());
  TASK_CHECK(right_inspection.members[1].type_source ==
             base->getOriginToken());

  // The rendered record names the module and the selected layout.
  auto text = left_inspection.toText();
  TASK_CHECK(contains(text, "module=left"));
  TASK_CHECK(contains(text, "size=16"));
  TASK_CHECK(contains(text, "source="));

  return 0;
}

static int compatibilityForensicsSizeMismatch() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);

  // 24 bytes submitted under the 16-byte left identity must fail loudly
  // instead of reinterpreting another module's layout.
  std::vector<std::uint8_t> mismatched(24, 0x5A);

  auto result = inspectSplitType(*left, "left", 4, mismatched);
  TASK_CHECK(result.failed());

  const auto &error = result.takeError().get();
  TASK_CHECK(error.code == BTFErrorInformation::Code::ObjectSizeMismatch);
  TASK_CHECK(error.opt_file_range.has_value());
  TASK_CHECK(error.opt_file_range->offset == 24);
  TASK_CHECK(error.opt_file_range->size == 16);

  return 0;
}

static int compatibilityForensicsBitfieldUnexplainable() {
  auto path = buildBitfieldFixture();
  auto base = load(path);

  // The struct is unnamed at ID2; inspect it with 4 bytes.
  std::vector<std::uint8_t> bytes(4, 0);
  auto result = inspectSplitType(*base, "bits", 2, bytes);
  TASK_CHECK(!result.failed());

  auto inspection = result.takeValue();
  TASK_CHECK(!inspection.explainable);
  TASK_CHECK(!inspection.unexplainable_reason.empty());
  TASK_CHECK(inspection.members.size() == 1);
  TASK_CHECK(inspection.members[0].is_bitfield);
  TASK_CHECK(inspection.members[0].raw_bytes.empty());
  TASK_CHECK(contains(inspection.toText(), "explainable=0"));

  return 0;
}

static int compatibilityForensicsIncompleteUnexplainable() {
  auto path = buildIncompleteFixture();
  auto base = load(path);

  std::vector<std::uint8_t> bytes(4, 0);
  auto result = inspectSplitType(*base, "incomplete", 2, bytes);
  TASK_CHECK(!result.failed());

  auto inspection = result.takeValue();
  TASK_CHECK(!inspection.explainable);
  TASK_CHECK(inspection.members.size() == 1);
  TASK_CHECK(inspection.members[0].incomplete);
  TASK_CHECK(inspection.members[0].raw_bytes.empty());

  return 0;
}

static int compatibilityForensicsUnknownTypeRejected() {
  auto base = load(samples().base);
  auto left = split(*base, samples().left);

  std::vector<std::uint8_t> bytes(16, 0);
  auto result = inspectSplitType(*left, "left", 99, bytes);
  TASK_CHECK(result.failed());
  TASK_CHECK(result.takeError().get().code ==
             BTFErrorInformation::Code::InvalidSplitTypeReference);

  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"scopes", "sibling_ids_distinct", scopesSiblingIDsAreDistinct},
    {"scopes", "siblings_same_id_different_type",
     scopesSiblingsSameIDDifferentType},
    {"scopes", "provenance_source_and_depth",
     scopesProvenanceRecordsSourceAndDepth},
    {"scopes", "string_section_retains_ancestors",
     scopesStringSectionRetainsAncestorBytes},
    {"scopes", "three_level_chain", scopesThreeLevelChain},
    {"scopes", "survives_parent_and_file_release",
     scopesSurviveParentAndFileRelease},
    {"scopes", "sibling_same_id_different_references",
     scopesSiblingSameIDDifferentReferences},

    {"declarations", "shared_base_declared_once",
     declarationsSharedBaseDeclaredOnce},
    {"declarations", "self_references_stay_local",
     declarationsSelfReferencesStayLocal},
    {"declarations", "module_order_independent",
     declarationsModuleOrderIndependent},
    {"declarations", "independent_bases_never_merge",
     declarationsIndependentBasesNeverMerge},
    {"declarations", "reject_duplicate_module_names",
     declarationsRejectDuplicateModuleNames},
    {"declarations", "reject_invalid_module_names",
     declarationsRejectInvalidModuleNames},
    {"declarations", "reject_missing_source_view",
     declarationsRejectMissingSourceView},
    {"declarations", "reject_null_view", declarationsRejectNullView},
    {"declarations", "enum_members_scoped", declarationsEnumMembersAreScoped},
    {"declarations", "fundamental_int_names_unchanged",
     declarationsFundamentalIntNamesUnchanged},
    {"declarations", "all_reference_kinds_rewritten",
     declarationsTypesReferencedByAllKinds},

    {"compatibility", "linear_interface_unchanged",
     compatibilityLinearInterfaceUnchanged},
    {"compatibility", "single_generate_unchanged",
     compatibilitySingleGenerateUnchanged},
    {"compatibility", "split_from_path_list_root",
     compatibilitySplitFromPathListRoot},
    {"compatibility", "missing_file_fails", compatibilityMissingFileFails},
    {"compatibility", "invalid_split_reference_rejected",
     compatibilityInvalidSplitReferenceRejected},
    {"compatibility", "forensics_distinguish_siblings",
     compatibilityForensicsDistinguishSiblings},
    {"compatibility", "forensics_size_mismatch",
     compatibilityForensicsSizeMismatch},
    {"compatibility", "forensics_bitfield_unexplainable",
     compatibilityForensicsBitfieldUnexplainable},
    {"compatibility", "forensics_incomplete_unexplainable",
     compatibilityForensicsIncompleteUnexplainable},
    {"compatibility", "forensics_unknown_type_rejected",
     compatibilityForensicsUnknownTypeRejected},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
