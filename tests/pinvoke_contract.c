/*
Contract tests for the dotnet module P/Invoke (ImplMap) support.

The test synthesizes minimal, metadata-only, benign PE files in memory,
compiles YARA rules that use dotnet.number_of_pinvokes / dotnet.pinvokes,
and scans them through the public yr_rules_scan_mem entry point. Nothing is
loaded or executed.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>
#include <yara/object.h>

#define PE_SIZE 4096

#define SECTION_RVA 0x1000
#define SECTION_RAW 0x200
#define SECTION_SIZE 0x800
#define CLI_RAW 0x200
#define CLI_RVA 0x1000
#define MD_ROOT_RAW 0x280
#define MD_ROOT_RVA 0x1080

#define METHOD_TOKEN_BASE 0x06000000

// Metadata table bit positions (ECMA-335 II.22).
#define T_MODULE 0x00
#define T_TYPEREF 0x01
#define T_TYPEDEF 0x02
#define T_FIELD 0x04
#define T_METHODDEF 0x06
#define T_PARAM 0x08
#define T_MODULEREF 0x1A
#define T_IMPLMAP 0x1C
#define T_NESTEDCLASS 0x29

// MemberForwarded coded index tags.
#define MF_TAG_FIELD 0x0
#define MF_TAG_METHODDEF 0x1

typedef struct _table_blob
{
  int table;
  uint32_t count;
  const uint8_t* rows;
  size_t rows_len;
} table_blob;

typedef struct _byte_blob
{
  uint8_t buf[2048];
  size_t len;
} byte_blob;

typedef struct _expected_pinvoke
{
  int64_t method_token;
  const char* managed_name;
  const char* type_name;
  const char* module_name;
  const char* import_name;
  int64_t mapping_flags;
} expected_pinvoke;

static void put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t* p, uint32_t v)
{
  put16(p, (uint16_t)(v & 0xFFFF));
  put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t* p, uint64_t v)
{
  put32(p, (uint32_t)(v & 0xFFFFFFFFULL));
  put32(p + 4, (uint32_t)(v >> 32));
}

static void blob_init(byte_blob* b)
{
  b->len = 0;
}

static void blob_bytes(byte_blob* b, const void* data, size_t n)
{
  assert(b->len + n <= sizeof(b->buf));
  memcpy(b->buf + b->len, data, n);
  b->len += n;
}

static void blob_u8(byte_blob* b, uint8_t v)
{
  blob_bytes(b, &v, 1);
}

static void blob_u16(byte_blob* b, uint16_t v)
{
  uint8_t tmp[2];
  put16(tmp, v);
  blob_bytes(b, tmp, sizeof(tmp));
}

static void blob_u32(byte_blob* b, uint32_t v)
{
  uint8_t tmp[4];
  put32(tmp, v);
  blob_bytes(b, tmp, sizeof(tmp));
}

// #Strings heap: index 0 is always the empty string.
static void strings_init(byte_blob* h)
{
  blob_u8(h, 0x00);
}

static uint32_t strings_add(byte_blob* h, const char* s)
{
  uint32_t index = (uint32_t) h->len;
  blob_bytes(h, s, strlen(s) + 1);
  return index;
}

// Append bytes without a terminating NUL, used for the malformed-string cases.
static uint32_t strings_add_unterminated(byte_blob* h, const char* s)
{
  uint32_t index = (uint32_t) h->len;
  blob_bytes(h, s, strlen(s));
  return index;
}

// #Blob heap: index 0 is the empty blob, followed by one shared method
// signature (2-byte signature: default calling convention, no parameters,
// void return).
static uint32_t blob_heap_build(byte_blob* h, int wide)
{
  static const uint8_t sig[] = {0x02, 0x00, 0x01};
  blob_u8(h, 0x00);
  uint32_t index = (uint32_t) h->len;
  blob_bytes(h, sig, sizeof(sig));
  (void) wide;
  return index;
}

// Builds a #~ stream: fixed header, row counts, then the caller-supplied,
// already packed table rows. Variable column widths are handled by the
// callers, which pick u16/u32 explicitly from the heap sizes.
static size_t tilde_build(
    uint8_t* out,
    uint8_t heap_sizes,
    const table_blob* tables,
    int n_tables)
{
  size_t p = 0;
  uint64_t valid = 0;

  for (int i = 0; i < n_tables; i++)
    valid |= 1ULL << tables[i].table;

  // Reserved1, MajorVersion, MinorVersion, HeapSizes, Reserved2.
  memset(out, 0, 24);
  out[4] = 2;
  out[5] = 0;
  out[6] = heap_sizes;
  put64(out + 8, valid);  // Valid
  put64(out + 16, 0);     // Sorted
  p = 24;

  for (int i = 0; i < n_tables; i++)
  {
    put32(out + p, tables[i].count);
    p += 4;
  }

  for (int i = 0; i < n_tables; i++)
  {
    memcpy(out + p, tables[i].rows, tables[i].rows_len);
    p += tables[i].rows_len;
  }

  return p;
}

// Writes the fixed PE32 shell used by every test case.
static void write_pe_shell(uint8_t* data, int with_com_descriptor)
{
  memset(data, 0xFF, PE_SIZE);

  // DOS header.
  put16(data, 0x5A4D);       // MZ
  put32(data + 0x3C, 0x80);  // e_lfanew

  // NT headers.
  put32(data + 0x80, 0x00004550);  // PE\0\0
  put16(data + 0x84, 0x014C);      // Machine: i386
  put16(data + 0x86, 1);           // NumberOfSections
  put32(data + 0x88, 0);           // TimeDateStamp
  put32(data + 0x8C, 0);           // PointerToSymbolTable
  put32(data + 0x90, 0);           // NumberOfSymbols
  put16(data + 0x94, 0);           // SizeOfOptionalHeader (filled below)
  put16(data + 0x96, 0x0102);      // Characteristics

  // Optional header (PE32).
  uint8_t* opt = data + 0x98;
  put16(opt, 0x010B);  // Magic
  opt[2] = 0;          // MajorLinkerVersion
  opt[3] = 0;          // MinorLinkerVersion
  put32(opt + 4, 0);   // SizeOfCode
  put32(opt + 8, 0);   // SizeOfInitializedData
  put32(opt + 12, 0);  // SizeOfUninitializedData
  put32(opt + 16, 0);  // AddressOfEntryPoint
  put32(opt + 20, 0);  // BaseOfCode
  put32(opt + 24, 0x400000);  // ImageBase
  put32(opt + 28, SECTION_SIZE);  // SectionAlignment
  put32(opt + 32, 0x200);         // FileAlignment
  put16(opt + 36, 4);             // MajorOperatingSystemVersion
  put16(opt + 38, 0);             // MinorOperatingSystemVersion
  put16(opt + 40, 0);             // MajorImageVersion
  put16(opt + 42, 0);             // MinorImageVersion
  put16(opt + 44, 4);             // MajorSubsystemVersion
  put16(opt + 46, 0);             // MinorSubsystemVersion
  put32(opt + 48, 0);             // Win32VersionValue
  put32(opt + 52, 0x4000);        // SizeOfImage
  put32(opt + 56, SECTION_RAW);   // SizeOfHeaders
  put32(opt + 60, 0);             // CheckSum
  put16(opt + 64, 3);             // Subsystem: CONSOLE
  put16(opt + 66, 0);             // DllCharacteristics
  put32(opt + 68, 0x100000);      // SizeOfStackReserve
  put32(opt + 72, 0x1000);        // SizeOfStackCommit
  put32(opt + 76, 0x100000);      // SizeOfHeapReserve
  put32(opt + 80, 0x1000);        // SizeOfHeapCommit
  put32(opt + 84, 0);             // LoaderFlags
  put32(opt + 88, 16);            // NumberOfRvaAndSizes
  put16(data + 0x94, 0xE0);       // SizeOfOptionalHeader

  // Data directory[14] = COM descriptor.
  if (with_com_descriptor)
  {
    put32(opt + 96 + 14 * 8, CLI_RVA);
    put32(opt + 96 + 14 * 8 + 4, 72);
  }

  // Single section header at 0x178.
  uint8_t* sec = data + 0x178;
  memcpy(sec, ".meta", 5);
  put32(sec + 8, SECTION_SIZE);   // VirtualSize
  put32(sec + 12, SECTION_RVA);   // VirtualAddress
  put32(sec + 16, SECTION_SIZE);  // SizeOfRawData
  put32(sec + 20, SECTION_RAW);   // PointerToRawData
  put32(sec + 24, 0);             // PointerToRelocations
  put32(sec + 28, 0);             // PointerToLinenumbers
  put16(sec + 32, 0);             // NumberOfRelocations
  put16(sec + 34, 0);             // NumberOfLinenumbers
  put32(sec + 36, 0x40000040);    // Characteristics
}

typedef struct _metadata_layout
{
  size_t total_size;
  size_t tilde_off;
  size_t strings_off;
  size_t blob_off;
  size_t tilde_len;
  size_t strings_len;
  size_t blob_len;
} metadata_layout;

// Writes CLI header, metadata root, stream headers and stream data. When
// tilde_last is nonzero the #~ stream is placed after #Strings/#Blob, which
// lets a truncated final table row sit at the very end of the scanned data.
static metadata_layout write_metadata_ex(
    uint8_t* data,
    const uint8_t* tilde,
    size_t tilde_len,
    const uint8_t* strings,
    size_t strings_len,
    const uint8_t* blob,
    size_t blob_len,
    int tilde_last)
{
  metadata_layout layout = {0};

  // CLI header.
  uint8_t* cli = data + CLI_RAW;
  put32(cli, 72);       // Cb
  put16(cli + 4, 2);    // MajorRuntimeVersion
  put16(cli + 6, 5);    // MinorRuntimeVersion

  static const char version[] = "v4.0.30319";
  const uint32_t version_field_len = 12;  // includes NUL and padding

  // Metadata root.
  uint8_t* root = data + MD_ROOT_RAW;
  put32(root, 0x424A5342);  // Magic "BSJB"
  put16(root + 4, 1);       // MajorVersion
  put16(root + 6, 1);       // MinorVersion
  put32(root + 8, 0);       // Reserved
  put32(root + 12, version_field_len);
  memcpy(root + 16, version, sizeof(version) - 1);
  // Version field is zero-initialized beyond the NUL by write_pe_shell's
  // 0xFF fill; make padding explicit.
  memset(root + 16 + sizeof(version) - 1, 0,
         version_field_len - (sizeof(version) - 1));
  put16(root + 16 + version_field_len, 0);  // Flags
  put16(root + 16 + version_field_len + 2, 3);  // Streams

  size_t header_len = 16 + version_field_len + 4;

  size_t tilde_name_pad = 4;  // "#~" + NUL
  size_t strings_name_pad = 12;  // "#Strings" + NUL, padded to 4
  size_t blob_name_pad = 8;      // "#Blob" + NUL, padded to 4

  size_t tilde_off, strings_off, blob_off, total;
  size_t p = header_len;

  if (!tilde_last)
  {
    tilde_off = header_len + 8 + tilde_name_pad + 8 +
                strings_name_pad + 8 + blob_name_pad;
    strings_off = tilde_off + tilde_len;
    blob_off = strings_off + strings_len;
    total = blob_off + blob_len;

    put32(root + p, (uint32_t) tilde_off);
    put32(root + p + 4, (uint32_t) tilde_len);
    memcpy(root + p + 8, "#~", 3);
    root[p + 10] = 0;
    p += 8 + tilde_name_pad;

    put32(root + p, (uint32_t) strings_off);
    put32(root + p + 4, (uint32_t) strings_len);
    memcpy(root + p + 8, "#Strings", 9);
    memset(root + p + 8 + 9, 0, strings_name_pad - 9);
    p += 8 + strings_name_pad;

    put32(root + p, (uint32_t) blob_off);
    put32(root + p + 4, (uint32_t) blob_len);
    memcpy(root + p + 8, "#Blob", 6);
    memset(root + p + 8 + 6, 0, blob_name_pad - 6);
    p += 8 + blob_name_pad;

    assert(p == tilde_off);

    memcpy(root + tilde_off, tilde, tilde_len);
    memcpy(root + strings_off, strings, strings_len);
    memcpy(root + blob_off, blob, blob_len);
  }
  else
  {
    // #Strings, #Blob, then #~ so the truncated table row is the final byte.
    strings_off = header_len + 8 + strings_name_pad + 8 +
                  blob_name_pad + 8 + tilde_name_pad;
    // strings start right after all three headers; compute backwards.
    size_t data_start =
        header_len + 8 + strings_name_pad + 8 + blob_name_pad + 8 +
        tilde_name_pad;
    strings_off = data_start;
    blob_off = strings_off + strings_len;
    tilde_off = blob_off + blob_len;
    total = tilde_off + tilde_len;

    put32(root + p, (uint32_t) strings_off);
    put32(root + p + 4, (uint32_t) strings_len);
    memcpy(root + p + 8, "#Strings", 9);
    memset(root + p + 8 + 9, 0, strings_name_pad - 9);
    p += 8 + strings_name_pad;

    put32(root + p, (uint32_t) blob_off);
    put32(root + p + 4, (uint32_t) blob_len);
    memcpy(root + p + 8, "#Blob", 6);
    memset(root + p + 8 + 6, 0, blob_name_pad - 6);
    p += 8 + blob_name_pad;

    put32(root + p, (uint32_t) tilde_off);
    put32(root + p + 4, (uint32_t) tilde_len);
    memcpy(root + p + 8, "#~", 3);
    root[p + 10] = 0;
    p += 8 + tilde_name_pad;

    assert(p == data_start);

    memcpy(root + strings_off, strings, strings_len);
    memcpy(root + blob_off, blob, blob_len);
    memcpy(root + tilde_off, tilde, tilde_len);
  }

  // CLI header MetaData directory points at the metadata root.
  put32(cli + 8, MD_ROOT_RVA);
  put32(cli + 12, (uint32_t) total);

  layout.total_size = total;
  layout.tilde_off = tilde_off;
  layout.strings_off = strings_off;
  layout.blob_off = blob_off;
  layout.tilde_len = tilde_len;
  layout.strings_len = strings_len;
  layout.blob_len = blob_len;

  (void) SECTION_RVA;
  return layout;
}

static metadata_layout write_metadata(
    uint8_t* data,
    const uint8_t* tilde,
    size_t tilde_len,
    const uint8_t* strings,
    size_t strings_len,
    const uint8_t* blob,
    size_t blob_len)
{
  return write_metadata_ex(
      data,
      tilde,
      tilde_len,
      strings,
      strings_len,
      blob,
      blob_len,
      0);
}

// Assembles a complete scan buffer from the given heaps.
static size_t build_pe(
    uint8_t* data,
    uint8_t heap_sizes,
    const table_blob* tables,
    int n_tables,
    const byte_blob* strings,
    const byte_blob* blob_heap)
{
  write_pe_shell(data, 1);

  byte_blob tilde;
  blob_init(&tilde);
  size_t tilde_len = tilde_build(
      tilde.buf, heap_sizes, tables, n_tables);
  tilde.len = tilde_len;

  write_metadata(
      data,
      tilde.buf,
      tilde.len,
      strings->buf,
      strings->len,
      blob_heap->buf,
      blob_heap->len);

  return PE_SIZE;
}

// A plain, non-managed PE: valid shell but no COM descriptor directory.
static size_t build_unmanaged_pe(uint8_t* data)
{
  write_pe_shell(data, 0);
  return PE_SIZE;
}

// Index sizes used while packing rows.
typedef struct _idx
{
  int str;
  int blob;
  int guid;
} idx;

static void str_index(byte_blob* b, const idx* ix, uint32_t v)
{
  if (ix->str == 4)
    blob_u32(b, v);
  else
    blob_u16(b, (uint16_t) v);
}

static void blob_index(byte_blob* b, const idx* ix, uint32_t v)
{
  if (ix->blob == 4)
    blob_u32(b, v);
  else
    blob_u16(b, (uint16_t) v);
}

static void guid_index(byte_blob* b, const idx* ix, uint32_t v)
{
  if (ix->guid == 4)
    blob_u32(b, v);
  else
    blob_u16(b, (uint16_t) v);
}

// MemberForwarded coded index: tag in the low bit.
static uint32_t member_forwarded(int tag, uint32_t row)
{
  return (row << 1) | (tag & 0x1);
}

static int g_compiler_errors = 0;

static void compiler_callback(
    int error_level,
    const char* file_name,
    int line_number,
    const YR_RULE* rule,
    const char* message,
    void* user_data)
{
  (void) error_level;
  (void) file_name;
  (void) line_number;
  (void) rule;
  (void) user_data;
  fprintf(stderr, "rule compile error: %s\n", message);
  g_compiler_errors++;
}

typedef struct _match_list
{
  char names[16][64];
  int count;
} match_list;

static int scan_callback(
    YR_SCAN_CONTEXT* context,
    int message,
    void* message_data,
    void* user_data)
{
  (void) context;
  if (message == CALLBACK_MSG_RULE_MATCHING)
  {
    YR_RULE* rule = (YR_RULE*) message_data;
    match_list* matches = (match_list*) user_data;
    if (matches->count < 16)
    {
      strncpy(matches->names[matches->count], rule->identifier, 63);
      matches->names[matches->count][63] = '\0';
      matches->count++;
    }
  }
  return CALLBACK_CONTINUE;
}

static int compile_and_scan(
    const char* rules_source,
    const uint8_t* data,
    size_t size,
    match_list* matches)
{
  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  matches->count = 0;
  g_compiler_errors = 0;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
    return -1;

  yr_compiler_set_callback(compiler, compiler_callback, NULL);

  if (yr_compiler_add_string(compiler, rules_source, NULL) != 0 ||
      g_compiler_errors != 0)
    goto fail;

  if (yr_compiler_get_rules(compiler, &rules) != ERROR_SUCCESS)
    goto fail;

  if (yr_rules_scan_mem(
          rules, data, size, 0, scan_callback, matches, 0) != ERROR_SUCCESS)
    goto fail;

  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
  return 0;

fail:
  if (rules != NULL)
    yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
  return -1;
}

static int has_match(const match_list* matches, const char* name)
{
  for (int i = 0; i < matches->count; i++)
    if (strcmp(matches->names[i], name) == 0)
      return 1;
  return 0;
}

#define MAX_PINVOKES 16

// Snapshot of a single pinvokes[] entry, taken while the module object is
// still alive (during CALLBACK_MSG_MODULE_IMPORTED).
typedef struct _pinvoke_snapshot
{
  int64_t method_token;
  int64_t mapping_flags;
  char managed_name[128];
  char type_name[128];
  char module_name[128];
  char import_name[128];
  int strings_present;
} pinvoke_snapshot;

typedef struct _module_snapshot
{
  int64_t is_dotnet;
  int64_t number_of_pinvokes;
  int64_t number_of_modulerefs;
  pinvoke_snapshot pinvokes[MAX_PINVOKES];
  int array_length;
  int captured;
} module_snapshot;

static void snapshot_copy_str(char* dst, size_t n, const SIZED_STRING* src)
{
  if (src == NULL)
  {
    dst[0] = '\0';
    return;
  }

  size_t len = src->length;
  if (len >= n)
    len = n - 1;
  memcpy(dst, src->c_string, len);
  dst[len] = '\0';
}

// Reads the full dotnet module state through the object API. Module objects
// are unloaded before rule-matching callbacks run, so callers invoke this at
// CALLBACK_MSG_MODULE_IMPORTED.
static void capture_snapshot(YR_OBJECT* module, module_snapshot* snap)
{
  memset(snap, 0, sizeof(*snap));
  snap->captured = 1;

  snap->is_dotnet = yr_object_get_integer(module, "is_dotnet");
  snap->number_of_pinvokes =
      yr_object_get_integer(module, "number_of_pinvokes");
  snap->number_of_modulerefs =
      yr_object_get_integer(module, "number_of_modulerefs");

  YR_OBJECT* array = yr_object_lookup(module, 0, "pinvokes");
  if (array != NULL)
  {
    snap->array_length = yr_object_array_length(array);
    int n = snap->array_length;
    if (n > MAX_PINVOKES)
      n = MAX_PINVOKES;

    for (int i = 0; i < n; i++)
    {
      pinvoke_snapshot* out = &snap->pinvokes[i];
      out->method_token = yr_object_get_integer(
          module, "pinvokes[%i].method_token", i);
      out->mapping_flags = yr_object_get_integer(
          module, "pinvokes[%i].mapping_flags", i);
      SIZED_STRING* managed = yr_object_get_string(
          module, "pinvokes[%i].managed_name", i);
      SIZED_STRING* type = yr_object_get_string(
          module, "pinvokes[%i].type_name", i);
      SIZED_STRING* mod_name = yr_object_get_string(
          module, "pinvokes[%i].module_name", i);
      SIZED_STRING* import = yr_object_get_string(
          module, "pinvokes[%i].import_name", i);

      out->strings_present =
          (managed != NULL && type != NULL && mod_name != NULL &&
           import != NULL);
      snapshot_copy_str(out->managed_name, sizeof(out->managed_name), managed);
      snapshot_copy_str(out->type_name, sizeof(out->type_name), type);
      snapshot_copy_str(out->module_name, sizeof(out->module_name), mod_name);
      snapshot_copy_str(out->import_name, sizeof(out->import_name), import);
    }
  }
}

// Verifies a captured snapshot against the expected full result set.
static int verify_snapshot(
    const module_snapshot* snap,
    const expected_pinvoke* expected,
    int expected_count)
{
  if (!snap->captured)
  {
    fprintf(stderr, "dotnet module was never imported\n");
    return 0;
  }

  if (snap->number_of_pinvokes != expected_count)
  {
    fprintf(stderr,
            "number_of_pinvokes = %lld, want %d\n",
            (long long) snap->number_of_pinvokes,
            expected_count);
    return 0;
  }

  if (snap->array_length != expected_count)
  {
    fprintf(stderr,
            "pinvokes array length = %d, want %d\n",
            snap->array_length,
            expected_count);
    return 0;
  }

  for (int i = 0; i < expected_count; i++)
  {
    const pinvoke_snapshot* got = &snap->pinvokes[i];
    if (!got->strings_present ||
        got->method_token != expected[i].method_token ||
        got->mapping_flags != expected[i].mapping_flags ||
        strcmp(got->managed_name, expected[i].managed_name) != 0 ||
        strcmp(got->type_name, expected[i].type_name) != 0 ||
        strcmp(got->module_name, expected[i].module_name) != 0 ||
        strcmp(got->import_name, expected[i].import_name) != 0)
    {
      fprintf(stderr,
              "pinvoke[%d] mismatch: got token=%lld flags=%lld "
              "managed=%s type=%s module=%s import=%s\n",
              i,
              (long long) got->method_token,
              (long long) got->mapping_flags,
              got->managed_name,
              got->type_name,
              got->module_name,
              got->import_name);
      return 0;
    }
  }

  return 1;
}

typedef struct _verify_ctx
{
  const expected_pinvoke* expected;
  int expected_count;
  module_snapshot snapshot;
  int ok;
} verify_ctx;

static int verify_callback(
    YR_SCAN_CONTEXT* context,
    int message,
    void* message_data,
    void* user_data)
{
  (void) context;
  verify_ctx* v = (verify_ctx*) user_data;

  if (message == CALLBACK_MSG_MODULE_IMPORTED &&
      strcmp(((YR_OBJECT*) message_data)->identifier, "dotnet") == 0)
  {
    capture_snapshot((YR_OBJECT*) message_data, &v->snapshot);
  }

  return CALLBACK_CONTINUE;
}

// Compiles the rule and, while scanning, cross-checks every exposed value.
static int run_verified_scan(
    const char* rules_source,
    const uint8_t* data,
    size_t size,
    const expected_pinvoke* expected,
    int expected_count)
{
  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;
  verify_ctx v;
  memset(&v, 0, sizeof(v));
  v.expected = expected;
  v.expected_count = expected_count;
  v.ok = 1;

  g_compiler_errors = 0;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
    return 0;
  yr_compiler_set_callback(compiler, compiler_callback, NULL);

  if (yr_compiler_add_string(compiler, rules_source, NULL) != 0 ||
      g_compiler_errors != 0)
    goto fail;
  if (yr_compiler_get_rules(compiler, &rules) != ERROR_SUCCESS)
    goto fail;
  if (yr_rules_scan_mem(
          rules, data, size, 0, verify_callback, &v, 0) != ERROR_SUCCESS)
    goto fail;

  if (!verify_snapshot(&v.snapshot, expected, expected_count))
    v.ok = 0;

  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
  return v.ok;

fail:
  if (rules != NULL)
    yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
  return 0;
}

// ---------------------------------------------------------------------------
// Test case 1: basic association.
// ---------------------------------------------------------------------------
static int test_basic(const idx* ix, int heap_sizes)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_app = strings_add(&strings, "App");
  uint32_t s_worker = strings_add(&strings, "Worker");
  uint32_t s_dowork = strings_add(&strings, "DoWork");
  uint32_t s_helper = strings_add(&strings, "Helper");
  uint32_t s_native = strings_add(&strings, "benign-native");
  uint32_t s_imp1 = strings_add(&strings, "NativeDoWork");
  uint32_t s_imp2 = strings_add(&strings, "NativeHelper");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, heap_sizes == 0x07);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);  // Generation
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  // <Module>
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);  // Extends
  blob_u16(&typedef_rows, 1);  // Field
  blob_u16(&typedef_rows, 1);  // Method
  // App.Worker
  blob_u32(&typedef_rows, 0x00000001);  // public class
  str_index(&typedef_rows, ix, s_worker);
  str_index(&typedef_rows, ix, s_app);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  blob_u32(&methoddef_rows, 0);  // Rva (no body)
  blob_u16(&methoddef_rows, 0);  // ImplFlags
  blob_u16(&methoddef_rows, 0x0006);  // Flags: public static
  str_index(&methoddef_rows, ix, s_dowork);
  blob_index(&methoddef_rows, ix, sig);
  blob_u16(&methoddef_rows, 0);  // ParamList
  blob_u32(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0x0006);
  str_index(&methoddef_rows, ix, s_helper);
  blob_index(&methoddef_rows, ix, sig);
  blob_u16(&methoddef_rows, 0);

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_native);

  byte_blob implmap_rows;
  blob_init(&implmap_rows);
  // Row 1: MethodDef 1 -> NativeDoWork, MappingFlags = 0x0001.
  blob_u16(&implmap_rows, 0x0001);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&implmap_rows, ix, s_imp1);
  blob_u16(&implmap_rows, 1);  // ImportScope (ModuleRef 1)
  // Row 2: MethodDef 2 -> NativeHelper, MappingFlags = 0x0007.
  blob_u16(&implmap_rows, 0x0007);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 2));
  str_index(&implmap_rows, ix, s_imp2);
  blob_u16(&implmap_rows, 1);

  table_blob tables[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 2, methoddef_rows.buf, methoddef_rows.len},
      {T_MODULEREF, 1, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 2, implmap_rows.buf, implmap_rows.len},
  };

  static uint8_t data[PE_SIZE];
  build_pe(data, (uint8_t) heap_sizes, tables, 5, &strings, &blob_heap);

  const char* rules =
      "import \"dotnet\" \
       rule basic_check { \
         condition: \
           dotnet.is_dotnet and \
           dotnet.number_of_pinvokes == 2 and \
           dotnet.pinvokes[0].method_token == 0x06000001 and \
           dotnet.pinvokes[0].managed_name == \"DoWork\" and \
           dotnet.pinvokes[0].type_name == \"App.Worker\" and \
           dotnet.pinvokes[0].module_name == \"benign-native\" and \
           dotnet.pinvokes[0].import_name == \"NativeDoWork\" and \
           dotnet.pinvokes[0].mapping_flags == 1 and \
           dotnet.pinvokes[1].method_token == 0x06000002 and \
           dotnet.pinvokes[1].managed_name == \"Helper\" and \
           dotnet.pinvokes[1].type_name == \"App.Worker\" and \
           dotnet.pinvokes[1].import_name == \"NativeHelper\" and \
           dotnet.pinvokes[1].mapping_flags == 7 \
       } \
       rule basic_wrong_count { \
         condition: dotnet.number_of_pinvokes == 3 \
       }";

  expected_pinvoke expected[] = {
      {METHOD_TOKEN_BASE + 1,
       "DoWork",
       "App.Worker",
       "benign-native",
       "NativeDoWork",
       1},
      {METHOD_TOKEN_BASE + 2,
       "Helper",
       "App.Worker",
       "benign-native",
       "NativeHelper",
       7},
  };

  match_list matches;
  if (compile_and_scan(rules, data, PE_SIZE, &matches) != 0)
    return 0;
  if (!has_match(&matches, "basic_check") ||
      has_match(&matches, "basic_wrong_count"))
    return 0;

  return run_verified_scan(
      rules, data, PE_SIZE, expected, (int) (sizeof(expected) /
                                             sizeof(expected[0])));
}

// ---------------------------------------------------------------------------
// Test case 2: same-named members and a nested type.
// ---------------------------------------------------------------------------
static int test_member_identity(const idx* ix)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns1 = strings_add(&strings, "Ns1");
  uint32_t s_ns2 = strings_add(&strings, "Ns2");
  uint32_t s_outer = strings_add(&strings, "Outer");
  uint32_t s_inner = strings_add(&strings, "Inner");
  uint32_t s_other = strings_add(&strings, "Other");
  uint32_t s_ping = strings_add(&strings, "Ping");
  uint32_t s_lib = strings_add(&strings, "liba.dll");
  uint32_t s_a = strings_add(&strings, "a");
  uint32_t s_b = strings_add(&strings, "b");
  uint32_t s_c = strings_add(&strings, "c");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 0);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  // <Module>
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  // Ns1.Outer (public)
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_outer);
  str_index(&typedef_rows, ix, s_ns1);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  // Ns1.Outer.Inner (nested public, no namespace string)
  blob_u32(&typedef_rows, 0x00000002);
  str_index(&typedef_rows, ix, s_inner);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 2);
  // Ns2.Other (public)
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_other);
  str_index(&typedef_rows, ix, s_ns2);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 3);

  byte_blob nested_rows;
  blob_init(&nested_rows);
  blob_u16(&nested_rows, 3);  // NestedClass
  blob_u16(&nested_rows, 2);  // EnclosingClass

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  for (int i = 0; i < 3; i++)
  {
    blob_u32(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0x0006);
    str_index(&methoddef_rows, ix, s_ping);
    blob_index(&methoddef_rows, ix, sig);
    blob_u16(&methoddef_rows, 0);
  }

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_lib);

  byte_blob implmap_rows;
  blob_init(&implmap_rows);
  uint32_t imports[3] = {s_a, s_b, s_c};
  for (int i = 0; i < 3; i++)
  {
    blob_u16(&implmap_rows, (uint16_t)(0x10 + i));
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, i + 1));
    str_index(&implmap_rows, ix, imports[i]);
    blob_u16(&implmap_rows, 1);
  }

  table_blob tables[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 4, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 3, methoddef_rows.buf, methoddef_rows.len},
      {T_MODULEREF, 1, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 3, implmap_rows.buf, implmap_rows.len},
      {T_NESTEDCLASS, 1, nested_rows.buf, nested_rows.len},
  };

  static uint8_t data[PE_SIZE];
  build_pe(data, 0x00, tables, 6, &strings, &blob_heap);

  const char* rules =
      "import \"dotnet\" \
       rule member_check { \
         condition: \
           dotnet.number_of_pinvokes == 3 and \
           dotnet.pinvokes[0].managed_name == \"Ping\" and \
           dotnet.pinvokes[0].type_name == \"Ns1.Outer\" and \
           dotnet.pinvokes[0].import_name == \"a\" and \
           dotnet.pinvokes[1].type_name == \"Ns1.Outer.Inner\" and \
           dotnet.pinvokes[1].import_name == \"b\" and \
           dotnet.pinvokes[2].type_name == \"Ns2.Other\" and \
           dotnet.pinvokes[2].import_name == \"c\" and \
           for any p in dotnet.pinvokes : ( \
             p.managed_name == \"Ping\" and p.module_name == \"liba.dll\") \
       }";

  expected_pinvoke expected[] = {
      {METHOD_TOKEN_BASE + 1, "Ping", "Ns1.Outer", "liba.dll", "a", 0x10},
      {METHOD_TOKEN_BASE + 2, "Ping", "Ns1.Outer.Inner", "liba.dll", "b",
       0x11},
      {METHOD_TOKEN_BASE + 3, "Ping", "Ns2.Other", "liba.dll", "c", 0x12},
  };

  return run_verified_scan(
      rules,
      data,
      PE_SIZE,
      expected,
      (int) (sizeof(expected) / sizeof(expected[0])));
}

// ---------------------------------------------------------------------------
// Test case 3: wide (#Strings/#GUID/#Blob) heaps.
// ---------------------------------------------------------------------------
static int test_wide_heaps(void)
{
  idx ix = {.str = 4, .blob = 4, .guid = 4};

  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);

  // Insert an unreferenced 300-byte name so real indexes exceed 0xFF,
  // proving the 4-byte index columns are actually consumed.
  char padding[301];
  memset(padding, 'x', sizeof(padding) - 1);
  padding[sizeof(padding) - 1] = '\0';
  strings_add(&strings, padding);

  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns = strings_add(&strings, "W");
  uint32_t s_wide = strings_add(&strings, "Wide");
  uint32_t s_call = strings_add(&strings, "Call");
  uint32_t s_lib = strings_add(&strings, "wide-native.dll");
  uint32_t s_import = strings_add(&strings, "W_Call");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 1);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, &ix, 0);
  guid_index(&module_rows, &ix, 0);
  guid_index(&module_rows, &ix, 0);
  guid_index(&module_rows, &ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, &ix, s_module);
  str_index(&typedef_rows, &ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, &ix, s_wide);
  str_index(&typedef_rows, &ix, s_ns);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  blob_u32(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0x0006);
  str_index(&methoddef_rows, &ix, s_call);
  blob_index(&methoddef_rows, &ix, sig);
  blob_u16(&methoddef_rows, 0);

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, &ix, s_lib);

  byte_blob implmap_rows;
  blob_init(&implmap_rows);
  blob_u16(&implmap_rows, 0x0010);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&implmap_rows, &ix, s_import);
  blob_u16(&implmap_rows, 1);

  table_blob tables[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 1, methoddef_rows.buf, methoddef_rows.len},
      {T_MODULEREF, 1, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 1, implmap_rows.buf, implmap_rows.len},
  };

  static uint8_t data[PE_SIZE];
  build_pe(data, 0x07, tables, 5, &strings, &blob_heap);

  const char* rules =
      "import \"dotnet\" \
       rule wide_check { \
         condition: \
           dotnet.number_of_pinvokes == 1 and \
           dotnet.pinvokes[0].method_token == 0x06000001 and \
           dotnet.pinvokes[0].managed_name == \"Call\" and \
           dotnet.pinvokes[0].type_name == \"W.Wide\" and \
           dotnet.pinvokes[0].module_name == \"wide-native.dll\" and \
           dotnet.pinvokes[0].import_name == \"W_Call\" and \
           dotnet.pinvokes[0].mapping_flags == 0x10 and \
           dotnet.modulerefs[0] == \"wide-native.dll\" \
       }";

  expected_pinvoke expected[] = {
      {METHOD_TOKEN_BASE + 1,
       "Call",
       "W.Wide",
       "wide-native.dll",
       "W_Call",
       0x10},
  };

  return run_verified_scan(
      rules,
      data,
      PE_SIZE,
      expected,
      (int) (sizeof(expected) / sizeof(expected[0])));
}

// Invalid rows interleaved with valid ones. Two fixtures are needed because a
// truncated final table row and an unterminated final heap string each have to
// sit at the end of the scanned data.
static int test_bad_rows(const idx* ix)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns = strings_add(&strings, "Bad");
  uint32_t s_class = strings_add(&strings, "Svc");
  uint32_t s_good1 = strings_add(&strings, "Good1");
  uint32_t s_good2 = strings_add(&strings, "Good2");
  uint32_t s_m1 = strings_add(&strings, "m1.dll");
  uint32_t s_m2 = strings_add(&strings, "m2.dll");
  uint32_t s_imp1 = strings_add(&strings, "imp1");
  uint32_t s_imp2 = strings_add(&strings, "imp2");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 0);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_class);
  str_index(&typedef_rows, ix, s_ns);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  uint32_t methods[2] = {s_good1, s_good2};
  for (int i = 0; i < 2; i++)
  {
    blob_u32(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0x0006);
    str_index(&methoddef_rows, ix, methods[i]);
    blob_index(&methoddef_rows, ix, sig);
    blob_u16(&methoddef_rows, 0);
  }

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_m1);
  str_index(&moduleref_rows, ix, s_m2);
  // ModuleRef row 3 points at a string index outside the #Strings heap.
  str_index(&moduleref_rows, ix, 0xFFFF);

  const char* rules =
      "import \"dotnet\" \
       rule bad_rows_check { \
         condition: \
           dotnet.number_of_pinvokes == 2 and \
           dotnet.pinvokes[0].method_token == 0x06000001 and \
           dotnet.pinvokes[0].managed_name == \"Good1\" and \
           dotnet.pinvokes[0].type_name == \"Bad.Svc\" and \
           dotnet.pinvokes[0].module_name == \"m1.dll\" and \
           dotnet.pinvokes[0].import_name == \"imp1\" and \
           dotnet.pinvokes[0].mapping_flags == 0x11 and \
           dotnet.pinvokes[1].method_token == 0x06000002 and \
           dotnet.pinvokes[1].managed_name == \"Good2\" and \
           dotnet.pinvokes[1].module_name == \"m2.dll\" and \
           dotnet.pinvokes[1].import_name == \"imp2\" and \
           dotnet.pinvokes[1].mapping_flags == 0x22 \
       } \
       rule bad_rows_no_extra { \
         condition: dotnet.number_of_pinvokes > 2 \
       }";

  expected_pinvoke expected[] = {
      {METHOD_TOKEN_BASE + 1, "Good1", "Bad.Svc", "m1.dll", "imp1", 0x11},
      {METHOD_TOKEN_BASE + 2, "Good2", "Bad.Svc", "m2.dll", "imp2", 0x22},
  };

  // ---- Fixture A: truncated final row, #~ placed last --------------------
  {
    byte_blob implmap_rows;
    blob_init(&implmap_rows);
    // 1: valid (method 1, imp1, scope 1).
    blob_u16(&implmap_rows, 0x11);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 1);
    // 2: zero MemberForwarded row number.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 0));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 1);
    // 3: Field tag instead of MethodDef.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_FIELD, 1));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 1);
    // 4: MethodDef row out of bounds.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 99));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 1);
    // 5: missing module (scope 0).
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 0);
    // 6: scope past ModuleRef table.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 4);
    // 7: import name index outside the #Strings heap.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&implmap_rows, ix, 0xFFFF);
    blob_u16(&implmap_rows, 1);
    // 8: ModuleRef whose name index is outside its heap.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&implmap_rows, ix, s_imp1);
    blob_u16(&implmap_rows, 3);
    // 9: valid (method 2, imp2, scope 2).
    blob_u16(&implmap_rows, 0x22);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 2));
    str_index(&implmap_rows, ix, s_imp2);
    blob_u16(&implmap_rows, 2);
    // 10: truncated row, only 4 of 8 bytes present.
    blob_u16(&implmap_rows, 0x01);
    blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));

    table_blob tables[] = {
        {T_MODULE, 1, module_rows.buf, module_rows.len},
        {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
        {T_METHODDEF, 2, methoddef_rows.buf, methoddef_rows.len},
        {T_MODULEREF, 3, moduleref_rows.buf, moduleref_rows.len},
        // Declare 10 rows; the last row's bytes are intentionally missing.
        {T_IMPLMAP, 10, implmap_rows.buf, implmap_rows.len},
    };

    byte_blob tilde;
    blob_init(&tilde);
    tilde.len = tilde_build(tilde.buf, 0x00, tables, 5);

    static uint8_t data[PE_SIZE];
    write_pe_shell(data, 1);
    metadata_layout layout = write_metadata_ex(
        data,
        tilde.buf,
        tilde.len,
        strings.buf,
        strings.len,
        blob_heap.buf,
        blob_heap.len,
        1);

    // Cut the scan through the partial final row; all preceding rows and both
    // heaps remain fully readable.
    size_t scan_size = MD_ROOT_RAW + layout.tilde_off + tilde.len;
    assert(scan_size < PE_SIZE);

    match_list matches;
    if (compile_and_scan(rules, data, scan_size, &matches) != 0)
      return 0;
    if (!has_match(&matches, "bad_rows_check") ||
        has_match(&matches, "bad_rows_no_extra"))
      return 0;

    if (!run_verified_scan(
            rules,
            data,
            scan_size,
            expected,
            (int) (sizeof(expected) / sizeof(expected[0]))))
      return 0;
  }

  // ---- Fixture B: unterminated string, #Strings placed last --------------
  {
    // A dedicated strings heap whose final entry has no NUL terminator.
    byte_blob bstrings;
    blob_init(&bstrings);
    strings_init(&bstrings);
    uint32_t b_module = strings_add(&bstrings, "<Module>");
    uint32_t b_ns = strings_add(&bstrings, "Bad");
    uint32_t b_class = strings_add(&bstrings, "Svc");
    uint32_t b_good = strings_add(&bstrings, "Good1");
    uint32_t b_lib = strings_add(&bstrings, "m1.dll");
    uint32_t b_imp = strings_add(&bstrings, "imp1");
    // Last entry: no terminating NUL anywhere before the end of the PE data.
    uint32_t b_bad_import =
        strings_add_unterminated(&bstrings, "dangling-import");

    byte_blob b_module_rows;
    blob_init(&b_module_rows);
    blob_u16(&b_module_rows, 0);
    str_index(&b_module_rows, ix, 0);
    guid_index(&b_module_rows, ix, 0);
    guid_index(&b_module_rows, ix, 0);
    guid_index(&b_module_rows, ix, 0);

    byte_blob b_typedef_rows;
    blob_init(&b_typedef_rows);
    blob_u32(&b_typedef_rows, 0);
    str_index(&b_typedef_rows, ix, b_module);
    str_index(&b_typedef_rows, ix, 0);
    blob_u16(&b_typedef_rows, 0);
    blob_u16(&b_typedef_rows, 1);
    blob_u16(&b_typedef_rows, 1);
    blob_u32(&b_typedef_rows, 0x00000001);
    str_index(&b_typedef_rows, ix, b_class);
    str_index(&b_typedef_rows, ix, b_ns);
    blob_u16(&b_typedef_rows, 0);
    blob_u16(&b_typedef_rows, 1);
    blob_u16(&b_typedef_rows, 1);

    byte_blob b_method_rows;
    blob_init(&b_method_rows);
    blob_u32(&b_method_rows, 0);
    blob_u16(&b_method_rows, 0);
    blob_u16(&b_method_rows, 0x0006);
    str_index(&b_method_rows, ix, b_good);
    blob_index(&b_method_rows, ix, sig);
    blob_u16(&b_method_rows, 0);

    byte_blob b_moduleref_rows;
    blob_init(&b_moduleref_rows);
    str_index(&b_moduleref_rows, ix, b_lib);

    byte_blob b_implmap_rows;
    blob_init(&b_implmap_rows);
    // Row 1: dangling import name with no NUL terminator -> must be skipped.
    blob_u16(&b_implmap_rows, 0x01);
    blob_u16(&b_implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&b_implmap_rows, ix, b_bad_import);
    blob_u16(&b_implmap_rows, 1);
    // Row 2: valid sibling mapping, must survive.
    blob_u16(&b_implmap_rows, 0x11);
    blob_u16(&b_implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
    str_index(&b_implmap_rows, ix, b_imp);
    blob_u16(&b_implmap_rows, 1);

    table_blob b_tables[] = {
        {T_MODULE, 1, b_module_rows.buf, b_module_rows.len},
        {T_TYPEDEF, 2, b_typedef_rows.buf, b_typedef_rows.len},
        {T_METHODDEF, 1, b_method_rows.buf, b_method_rows.len},
        {T_MODULEREF, 1, b_moduleref_rows.buf, b_moduleref_rows.len},
        {T_IMPLMAP, 2, b_implmap_rows.buf, b_implmap_rows.len},
    };

    byte_blob tilde;
    blob_init(&tilde);
    tilde.len = tilde_build(tilde.buf, 0x00, b_tables, 5);

    static uint8_t b_data[PE_SIZE];
    write_pe_shell(b_data, 1);

    // Custom stream order: #~, #Blob, #Strings so the unterminated string is
    // the final byte of metadata, followed only by NUL-free 0xFF padding.
    uint8_t* root = b_data + MD_ROOT_RAW;
    static const char version[] = "v4.0.30319";
    const uint32_t version_field_len = 12;
    put32(root, 0x424A5342);
    put16(root + 4, 1);
    put16(root + 6, 1);
    put32(root + 8, 0);
    put32(root + 12, version_field_len);
    memcpy(root + 16, version, sizeof(version) - 1);
    memset(root + 16 + sizeof(version) - 1,
           0,
           version_field_len - (sizeof(version) - 1));
    put16(root + 16 + version_field_len, 0);
    put16(root + 16 + version_field_len + 2, 3);

    size_t header_len = 16 + version_field_len + 4;
    size_t data_start = header_len + (8 + 4) + (8 + 8) + (8 + 12);
    size_t tilde_off = data_start;
    size_t blob_off = tilde_off + tilde.len;
    size_t strings_off = blob_off + blob_heap.len;

    size_t p = header_len;
    put32(root + p, (uint32_t) tilde_off);
    put32(root + p + 4, (uint32_t) tilde.len);
    memcpy(root + p + 8, "#~", 3);
    root[p + 10] = 0;
    p += 12;

    put32(root + p, (uint32_t) blob_off);
    put32(root + p + 4, (uint32_t) blob_heap.len);
    memcpy(root + p + 8, "#Blob", 6);
    root[p + 14] = 0;
    root[p + 15] = 0;
    p += 16;

    put32(root + p, (uint32_t) strings_off);
    put32(root + p + 4, (uint32_t) bstrings.len);
    memcpy(root + p + 8, "#Strings", 9);
    memset(root + p + 17, 0, 3);
    p += 20;
    assert(p == data_start);

    memcpy(root + tilde_off, tilde.buf, tilde.len);
    memcpy(root + blob_off, blob_heap.buf, blob_heap.len);
    memcpy(root + strings_off, bstrings.buf, bstrings.len);

    uint8_t* cli = b_data + CLI_RAW;
    put32(cli, 72);
    put16(cli + 4, 2);
    put16(cli + 6, 5);
    put32(cli + 8, MD_ROOT_RVA);
    put32(cli + 12, (uint32_t)(strings_off + bstrings.len));

    const char* b_rules =
        "import \"dotnet\" \
         rule dangling_check { \
           condition: \
             dotnet.number_of_pinvokes == 1 and \
             dotnet.pinvokes[0].managed_name == \"Good1\" and \
             dotnet.pinvokes[0].import_name == \"imp1\" \
         } \
         rule dangling_no_extra { \
           condition: dotnet.number_of_pinvokes != 1 \
         }";

    expected_pinvoke b_expected[] = {
        {METHOD_TOKEN_BASE + 1, "Good1", "Bad.Svc", "m1.dll", "imp1", 0x11},
    };

    match_list b_matches;
    if (compile_and_scan(b_rules, b_data, PE_SIZE, &b_matches) != 0)
      return 0;
    if (!has_match(&b_matches, "dangling_check") ||
        has_match(&b_matches, "dangling_no_extra"))
      return 0;

    if (!run_verified_scan(
            b_rules,
            b_data,
            PE_SIZE,
            b_expected,
            (int) (sizeof(b_expected) / sizeof(b_expected[0]))))
      return 0;
  }

  return 1;
}

// ---------------------------------------------------------------------------
// Test case 5: duplicate mappings for the same method are not de-duplicated.
// ---------------------------------------------------------------------------
static int test_duplicate_imports(const idx* ix)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns = strings_add(&strings, "Dup");
  uint32_t s_class = strings_add(&strings, "Api");
  uint32_t s_dup = strings_add(&strings, "Dup");
  uint32_t s_single = strings_add(&strings, "Single");
  uint32_t s_lib = strings_add(&strings, "dup.dll");
  uint32_t s_d1 = strings_add(&strings, "d1");
  uint32_t s_d2 = strings_add(&strings, "d2");
  uint32_t s_s1 = strings_add(&strings, "s1");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 0);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_class);
  str_index(&typedef_rows, ix, s_ns);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  uint32_t method_names[2] = {s_dup, s_single};
  for (int i = 0; i < 2; i++)
  {
    blob_u32(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0);
    blob_u16(&methoddef_rows, 0x0006);
    str_index(&methoddef_rows, ix, method_names[i]);
    blob_index(&methoddef_rows, ix, sig);
    blob_u16(&methoddef_rows, 0);
  }

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_lib);

  byte_blob implmap_rows;
  blob_init(&implmap_rows);
  // Two valid mappings on method 1, kept in physical order.
  blob_u16(&implmap_rows, 0x01);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&implmap_rows, ix, s_d1);
  blob_u16(&implmap_rows, 1);
  blob_u16(&implmap_rows, 0x02);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&implmap_rows, ix, s_d2);
  blob_u16(&implmap_rows, 1);
  // Then an unrelated method.
  blob_u16(&implmap_rows, 0x03);
  blob_u16(&implmap_rows, member_forwarded(MF_TAG_METHODDEF, 2));
  str_index(&implmap_rows, ix, s_s1);
  blob_u16(&implmap_rows, 1);

  table_blob tables[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 2, methoddef_rows.buf, methoddef_rows.len},
      {T_MODULEREF, 1, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 3, implmap_rows.buf, implmap_rows.len},
  };

  static uint8_t data[PE_SIZE];
  build_pe(data, 0x00, tables, 5, &strings, &blob_heap);

  const char* rules =
      "import \"dotnet\" \
       rule duplicate_check { \
         condition: \
           dotnet.number_of_pinvokes == 3 and \
           dotnet.pinvokes[0].method_token == 0x06000001 and \
           dotnet.pinvokes[0].import_name == \"d1\" and \
           dotnet.pinvokes[1].method_token == 0x06000001 and \
           dotnet.pinvokes[1].import_name == \"d2\" and \
           dotnet.pinvokes[2].method_token == 0x06000002 and \
           dotnet.pinvokes[2].managed_name == \"Single\" and \
           dotnet.pinvokes[2].import_name == \"s1\" \
       }";

  expected_pinvoke expected[] = {
      {METHOD_TOKEN_BASE + 1, "Dup", "Dup.Api", "dup.dll", "d1", 1},
      {METHOD_TOKEN_BASE + 1, "Dup", "Dup.Api", "dup.dll", "d2", 2},
      {METHOD_TOKEN_BASE + 2, "Single", "Dup.Api", "dup.dll", "s1", 3},
  };

  return run_verified_scan(
      rules,
      data,
      PE_SIZE,
      expected,
      (int) (sizeof(expected) / sizeof(expected[0])));
}

// ---------------------------------------------------------------------------
// Test case 6: no ImplMap table and non-managed input.
// ---------------------------------------------------------------------------
static int test_empty(const idx* ix)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns = strings_add(&strings, "Empty");
  uint32_t s_class = strings_add(&strings, "Plain");
  uint32_t s_method = strings_add(&strings, "ManagedOnly");
  uint32_t s_lib = strings_add(&strings, "unused-native.dll");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 0);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_class);
  str_index(&typedef_rows, ix, s_ns);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob methoddef_rows;
  blob_init(&methoddef_rows);
  blob_u32(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0);
  blob_u16(&methoddef_rows, 0x0006);
  str_index(&methoddef_rows, ix, s_method);
  blob_index(&methoddef_rows, ix, sig);
  blob_u16(&methoddef_rows, 0);

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_lib);

  // No ImplMap table at all.
  table_blob tables[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 1, methoddef_rows.buf, methoddef_rows.len},
      {T_MODULEREF, 1, moduleref_rows.buf, moduleref_rows.len},
  };

  static uint8_t managed[PE_SIZE];
  build_pe(managed, 0x00, tables, 4, &strings, &blob_heap);

  static uint8_t unmanaged[PE_SIZE];
  build_unmanaged_pe(unmanaged);

  const char* rules =
      "import \"dotnet\" \
       rule empty_managed { \
         condition: \
           dotnet.is_dotnet and \
           dotnet.number_of_pinvokes == 0 and \
           not for any p in dotnet.pinvokes : ( true ) and \
           dotnet.number_of_modulerefs == 1 \
       } \
       rule empty_unmanaged { \
         condition: \
           not dotnet.is_dotnet and \
           not for any p in dotnet.pinvokes : ( true ) \
       }";

  match_list matches;
  if (compile_and_scan(rules, managed, PE_SIZE, &matches) != 0)
    return 0;
  if (!has_match(&matches, "empty_managed"))
    return 0;

  match_list matches2;
  if (compile_and_scan(rules, unmanaged, PE_SIZE, &matches2) != 0)
    return 0;
  if (!has_match(&matches2, "empty_unmanaged"))
    return 0;

  return 1;
}

// ---------------------------------------------------------------------------
// Test case 7: one compiled rule set scanned against different inputs must
// not carry array items over between scans.
// ---------------------------------------------------------------------------
static int test_repeat_scan(const idx* ix)
{
  byte_blob strings;
  blob_init(&strings);
  strings_init(&strings);
  uint32_t s_module = strings_add(&strings, "<Module>");
  uint32_t s_ns = strings_add(&strings, "Rep");
  uint32_t s_class = strings_add(&strings, "Svc");
  uint32_t s_one = strings_add(&strings, "One");
  uint32_t s_two = strings_add(&strings, "Two");
  uint32_t s_lib1 = strings_add(&strings, "first.dll");
  uint32_t s_lib2 = strings_add(&strings, "second.dll");
  uint32_t s_i1 = strings_add(&strings, "first_import");
  uint32_t s_i2 = strings_add(&strings, "second_import");

  byte_blob blob_heap;
  blob_init(&blob_heap);
  uint32_t sig = blob_heap_build(&blob_heap, 0);

  byte_blob module_rows;
  blob_init(&module_rows);
  blob_u16(&module_rows, 0);
  str_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);
  guid_index(&module_rows, ix, 0);

  byte_blob typedef_rows;
  blob_init(&typedef_rows);
  blob_u32(&typedef_rows, 0);
  str_index(&typedef_rows, ix, s_module);
  str_index(&typedef_rows, ix, 0);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);
  blob_u32(&typedef_rows, 0x00000001);
  str_index(&typedef_rows, ix, s_class);
  str_index(&typedef_rows, ix, s_ns);
  blob_u16(&typedef_rows, 0);
  blob_u16(&typedef_rows, 1);
  blob_u16(&typedef_rows, 1);

  byte_blob moduleref_rows;
  blob_init(&moduleref_rows);
  str_index(&moduleref_rows, ix, s_lib1);
  str_index(&moduleref_rows, ix, s_lib2);

  // First input: two mappings.
  byte_blob methods_two;
  blob_init(&methods_two);
  uint32_t two_names[2] = {s_one, s_two};
  for (int i = 0; i < 2; i++)
  {
    blob_u32(&methods_two, 0);
    blob_u16(&methods_two, 0);
    blob_u16(&methods_two, 0x0006);
    str_index(&methods_two, ix, two_names[i]);
    blob_index(&methods_two, ix, sig);
    blob_u16(&methods_two, 0);
  }

  byte_blob impls_two;
  blob_init(&impls_two);
  blob_u16(&impls_two, 0x01);
  blob_u16(&impls_two, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&impls_two, ix, s_i1);
  blob_u16(&impls_two, 1);
  blob_u16(&impls_two, 0x02);
  blob_u16(&impls_two, member_forwarded(MF_TAG_METHODDEF, 2));
  str_index(&impls_two, ix, s_i2);
  blob_u16(&impls_two, 2);

  table_blob tables_two[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 2, methods_two.buf, methods_two.len},
      {T_MODULEREF, 2, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 2, impls_two.buf, impls_two.len},
  };

  // Second input: a single mapping (only method One, only first.dll).
  byte_blob methods_one;
  blob_init(&methods_one);
  blob_u32(&methods_one, 0);
  blob_u16(&methods_one, 0);
  blob_u16(&methods_one, 0x0006);
  str_index(&methods_one, ix, s_one);
  blob_index(&methods_one, ix, sig);
  blob_u16(&methods_one, 0);

  byte_blob impls_one;
  blob_init(&impls_one);
  blob_u16(&impls_one, 0x05);
  blob_u16(&impls_one, member_forwarded(MF_TAG_METHODDEF, 1));
  str_index(&impls_one, ix, s_i1);
  blob_u16(&impls_one, 1);

  table_blob tables_one[] = {
      {T_MODULE, 1, module_rows.buf, module_rows.len},
      {T_TYPEDEF, 2, typedef_rows.buf, typedef_rows.len},
      {T_METHODDEF, 1, methods_one.buf, methods_one.len},
      {T_MODULEREF, 2, moduleref_rows.buf, moduleref_rows.len},
      {T_IMPLMAP, 1, impls_one.buf, impls_one.len},
  };

  static uint8_t first[PE_SIZE];
  build_pe(first, 0x00, tables_two, 5, &strings, &blob_heap);
  static uint8_t second[PE_SIZE];
  build_pe(second, 0x00, tables_one, 5, &strings, &blob_heap);

  const char* rules =
      "import \"dotnet\" \
       rule repeat_two { \
         condition: dotnet.number_of_pinvokes == 2 \
       } \
       rule repeat_one { \
         condition: \
           dotnet.number_of_pinvokes == 1 and \
           dotnet.pinvokes[0].managed_name == \"One\" and \
           dotnet.pinvokes[0].module_name == \"first.dll\" and \
           dotnet.pinvokes[0].import_name == \"first_import\" and \
           dotnet.pinvokes[0].mapping_flags == 5 \
       }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* yr_rules = NULL;
  g_compiler_errors = 0;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
    return 0;
  yr_compiler_set_callback(compiler, compiler_callback, NULL);
  if (yr_compiler_add_string(compiler, rules, NULL) != 0 ||
      g_compiler_errors != 0)
    goto fail;
  if (yr_compiler_get_rules(compiler, &yr_rules) != ERROR_SUCCESS)
    goto fail;

  match_list first_matches;
  first_matches.count = 0;
  if (yr_rules_scan_mem(
          yr_rules, first, PE_SIZE, 0, scan_callback, &first_matches, 0) !=
      ERROR_SUCCESS)
    goto fail;
  if (!has_match(&first_matches, "repeat_two") ||
      has_match(&first_matches, "repeat_one"))
    goto fail;

  expected_pinvoke second_expected[] = {
      {METHOD_TOKEN_BASE + 1,
       "One",
       "Rep.Svc",
       "first.dll",
       "first_import",
       5},
  };

  verify_ctx v;
  memset(&v, 0, sizeof(v));
  v.expected = second_expected;
  v.expected_count = 1;
  v.ok = 1;
  if (yr_rules_scan_mem(
          yr_rules, second, PE_SIZE, 0, verify_callback, &v, 0) !=
      ERROR_SUCCESS)
    goto fail;
  if (!verify_snapshot(&v.snapshot, second_expected, 1))
    goto fail;

  match_list second_matches;
  second_matches.count = 0;
  if (yr_rules_scan_mem(
          yr_rules, second, PE_SIZE, 0, scan_callback, &second_matches, 0) !=
      ERROR_SUCCESS)
    goto fail;
  if (!has_match(&second_matches, "repeat_one") ||
      has_match(&second_matches, "repeat_two"))
    goto fail;

  yr_rules_destroy(yr_rules);
  yr_compiler_destroy(compiler);
  return 1;

fail:
  if (yr_rules != NULL)
    yr_rules_destroy(yr_rules);
  yr_compiler_destroy(compiler);
  return 0;
}

static void report(const char* name, int ok)
{
  if (ok)
    printf("PASS %s\n", name);
  else
    printf("FAIL %s\n", name);
}

int main(void)
{
  idx narrow = {.str = 2, .blob = 2, .guid = 2};

  if (yr_initialize() != ERROR_SUCCESS)
  {
    fprintf(stderr, "yr_initialize failed\n");
    return 1;
  }

  int ok_basic = test_basic(&narrow, 0x00);
  report("basic", ok_basic);

  int ok_member = test_member_identity(&narrow);
  report("member_identity", ok_member);

  int ok_wide = test_wide_heaps();
  report("wide_heaps", ok_wide);

  int ok_bad = test_bad_rows(&narrow);
  report("bad_rows", ok_bad);

  int ok_dup = test_duplicate_imports(&narrow);
  report("duplicate_imports", ok_dup);

  int ok_empty = test_empty(&narrow);
  report("empty", ok_empty);

  int ok_repeat = test_repeat_scan(&narrow);
  report("repeat_scan", ok_repeat);

  yr_finalize();

  return (ok_basic && ok_member && ok_wide && ok_bad && ok_dup &&
          ok_empty && ok_repeat)
             ? 0
             : 1;
}
