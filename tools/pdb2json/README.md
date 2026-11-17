# pdb2json

Command-line (and drag-and-drop) tool that turns a Microsoft `.pdb` into a folder
of UTF-8 JSON files. Built on the MemPDB library; it always parses with
`ParseOptions::Everything()`.

## What it does

1. Reads a PDB from disk.
2. Parses functions, arguments, structs, and globals.
3. Writes `<pdb-stem>_dump/` next to the input file with one JSON file per table
   (plus `metadata.json` and `index.json`).
4. Computes virtual addresses as `va = image_base + rva`. PDBs do not store the
   real runtime base (ASLR), so the default image base is `0x180000000` (typical
   x64 DLL preferred base). Override it on the command line.

Category files under `globals_*.json` are written only when that category is
non-empty.

Public / stripped PDBs (for example Microsoft symbol-server downloads) usually
have `size: 0` and empty `args` arrays. Argument names/types and proc sizes need
a private PDB built with full debug info.

## Usage

```
pdb2json <file.pdb> [imageBaseHex]
```

Examples:

```
pdb2json ntdll.pdb
pdb2json ntdll.pdb 0x140000000
```

On Windows you can also drop a `.pdb` onto `pdb2json.exe` in Explorer.

Set `MEMPDB_NO_PAUSE=1` to skip the "Press Enter to close" prompt (useful in
scripts).

## Output layout

```
<name>_dump/
  index.json
  metadata.json
  functions.json
  function_args.json
  structures.json
  globals.json
  globals_variables.json    (optional)
  globals_vtables.json      (optional)
  globals_vbtables.json     (optional)
  globals_rtti.json         (optional)
  globals_strings.json      (optional)
  globals_constants.json    (optional)
```

## Build

From the MemPDB repo root:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target pdb2json
```

Binary: `build/Release/pdb2json.exe` (or `build/pdb2json` on Linux).

---

## JSON schemas

Field types below match what the tool actually emits (integers are JSON numbers;
hex fields are strings). Schemas use JSON Schema draft 2020-12 style.

### Shared definitions

```json
{
  "$defs": {
    "hexString": {
      "type": "string",
      "pattern": "^0x[0-9A-Fa-f]+$"
    },
    "symbolKind": {
      "type": "string",
      "enum": [
        "Variable",
        "Constant",
        "Vtable",
        "VbTable",
        "Rtti",
        "StringLiteral",
        "Unknown"
      ]
    },
    "argument": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "type", "size", "offset"],
      "properties": {
        "name":   { "type": "string" },
        "type":   { "type": "string" },
        "size":   { "type": "integer", "minimum": 0 },
        "offset": { "type": "integer", "minimum": 0 }
      }
    },
    "field": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "type", "offset", "offset_hex", "size"],
      "properties": {
        "name":       { "type": "string" },
        "type":       { "type": "string" },
        "offset":     { "type": "integer", "minimum": 0 },
        "offset_hex": { "$ref": "#/$defs/hexString" },
        "size":       { "type": "integer", "minimum": 0 }
      }
    },
    "global": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "rva", "va", "rva_hex", "va_hex", "kind"],
      "properties": {
        "name":    { "type": "string" },
        "rva":     { "type": "integer", "minimum": 0 },
        "va":      { "type": "integer", "minimum": 0 },
        "rva_hex": { "$ref": "#/$defs/hexString" },
        "va_hex":  { "$ref": "#/$defs/hexString" },
        "kind":    { "$ref": "#/$defs/symbolKind" }
      }
    },
    "indexFileEntry": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "description"],
      "properties": {
        "name":        { "type": "string" },
        "description": { "type": "string" },
        "records":     { "type": "integer", "minimum": 0 }
      }
    }
  }
}
```

### `functions.json`

Array of functions (name, addresses, size).

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/functions.json",
  "title": "pdb2json functions.json",
  "type": "array",
  "items": {
    "type": "object",
    "additionalProperties": false,
    "required": ["name", "rva", "va", "rva_hex", "va_hex", "size"],
    "properties": {
      "name":    { "type": "string" },
      "rva":     { "type": "integer", "minimum": 0 },
      "va":      { "type": "integer", "minimum": 0 },
      "rva_hex": { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
      "va_hex":  { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
      "size":    { "type": "integer", "minimum": 0 }
    }
  }
}
```

Example item:

```json
{
  "name": "NtCreateFile",
  "rva": 647728,
  "va": 6443098672,
  "rva_hex": "0x9E1B0",
  "va_hex": "0x1809E1B0",
  "size": 0
}
```

### `function_args.json`

One object per function, including its argument list.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/function_args.json",
  "title": "pdb2json function_args.json",
  "type": "array",
  "items": {
    "type": "object",
    "additionalProperties": false,
    "required": ["name", "rva", "va", "arg_count", "args"],
    "properties": {
      "name":      { "type": "string" },
      "rva":       { "type": "integer", "minimum": 0 },
      "va":        { "type": "integer", "minimum": 0 },
      "arg_count": { "type": "integer", "minimum": 0 },
      "args": {
        "type": "array",
        "items": {
          "type": "object",
          "additionalProperties": false,
          "required": ["name", "type", "size", "offset"],
          "properties": {
            "name":   { "type": "string" },
            "type":   { "type": "string" },
            "size":   { "type": "integer", "minimum": 0 },
            "offset": { "type": "integer", "minimum": 0 }
          }
        }
      }
    }
  }
}
```

### `structures.json`

Struct / class / union layouts.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/structures.json",
  "title": "pdb2json structures.json",
  "type": "array",
  "items": {
    "type": "object",
    "additionalProperties": false,
    "required": ["name", "size", "field_count", "fields"],
    "properties": {
      "name":        { "type": "string" },
      "size":        { "type": "integer", "minimum": 0 },
      "field_count": { "type": "integer", "minimum": 0 },
      "fields": {
        "type": "array",
        "items": {
          "type": "object",
          "additionalProperties": false,
          "required": ["name", "type", "offset", "offset_hex", "size"],
          "properties": {
            "name":       { "type": "string" },
            "type":       { "type": "string" },
            "offset":     { "type": "integer", "minimum": 0 },
            "offset_hex": { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
            "size":       { "type": "integer", "minimum": 0 }
          }
        }
      }
    }
  }
}
```

### `globals.json` and `globals_*.json`

All of these share the same item schema. Filenames:

| File | `kind` filter |
|------|----------------|
| `globals.json` | all kinds |
| `globals_variables.json` | `Variable` |
| `globals_constants.json` | `Constant` |
| `globals_vtables.json` | `Vtable` |
| `globals_vbtables.json` | `VbTable` |
| `globals_rtti.json` | `Rtti` |
| `globals_strings.json` | `StringLiteral` |

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/globals.json",
  "title": "pdb2json globals.json",
  "type": "array",
  "items": {
    "type": "object",
    "additionalProperties": false,
    "required": ["name", "rva", "va", "rva_hex", "va_hex", "kind"],
    "properties": {
      "name":    { "type": "string" },
      "rva":     { "type": "integer", "minimum": 0 },
      "va":      { "type": "integer", "minimum": 0 },
      "rva_hex": { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
      "va_hex":  { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
      "kind": {
        "type": "string",
        "enum": [
          "Variable",
          "Constant",
          "Vtable",
          "VbTable",
          "Rtti",
          "StringLiteral",
          "Unknown"
        ]
      }
    }
  }
}
```

### `metadata.json`

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/metadata.json",
  "title": "pdb2json metadata.json",
  "type": "object",
  "additionalProperties": false,
  "required": [
    "tool",
    "generated",
    "source_file",
    "source_bytes",
    "image_base",
    "image_base_hex",
    "va_formula",
    "counts",
    "parse_ms",
    "parse_counts",
    "memory"
  ],
  "properties": {
    "tool":           { "type": "string" },
    "generated":      { "type": "string" },
    "source_file":    { "type": "string" },
    "source_bytes":   { "type": "integer", "minimum": 0 },
    "image_base":     { "type": "integer", "minimum": 0 },
    "image_base_hex": { "type": "string", "pattern": "^0x[0-9A-Fa-f]+$" },
    "va_formula":     { "type": "string" },
    "counts": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "functions",
        "arguments",
        "structs",
        "fields",
        "globals",
        "globals_by_kind"
      ],
      "properties": {
        "functions": { "type": "integer", "minimum": 0 },
        "arguments": { "type": "integer", "minimum": 0 },
        "structs":   { "type": "integer", "minimum": 0 },
        "fields":    { "type": "integer", "minimum": 0 },
        "globals":   { "type": "integer", "minimum": 0 },
        "globals_by_kind": {
          "type": "object",
          "additionalProperties": false,
          "required": [
            "Variable",
            "Constant",
            "Vtable",
            "VbTable",
            "Rtti",
            "StringLiteral",
            "Unknown"
          ],
          "properties": {
            "Variable":      { "type": "integer", "minimum": 0 },
            "Constant":      { "type": "integer", "minimum": 0 },
            "Vtable":        { "type": "integer", "minimum": 0 },
            "VbTable":       { "type": "integer", "minimum": 0 },
            "Rtti":          { "type": "integer", "minimum": 0 },
            "StringLiteral": { "type": "integer", "minimum": 0 },
            "Unknown":       { "type": "integer", "minimum": 0 }
          }
        }
      }
    },
    "parse_ms": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "msf_dbi",
        "tpi",
        "public_symbols",
        "sort",
        "module_streams",
        "structs",
        "intern",
        "total"
      ],
      "properties": {
        "msf_dbi":        { "type": "number" },
        "tpi":            { "type": "number" },
        "public_symbols": { "type": "number" },
        "sort":           { "type": "number" },
        "module_streams": { "type": "number" },
        "structs":        { "type": "number" },
        "intern":         { "type": "number" },
        "total":          { "type": "number" }
      }
    },
    "parse_counts": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "tpi_types",
        "public_symbols",
        "modules",
        "module_bytes",
        "procs_parsed",
        "proc_matches",
        "structs_decoded"
      ],
      "properties": {
        "tpi_types":       { "type": "integer", "minimum": 0 },
        "public_symbols":  { "type": "integer", "minimum": 0 },
        "modules":         { "type": "integer", "minimum": 0 },
        "module_bytes":    { "type": "integer", "minimum": 0 },
        "procs_parsed":    { "type": "integer", "minimum": 0 },
        "proc_matches":    { "type": "integer", "minimum": 0 },
        "structs_decoded": { "type": "integer", "minimum": 0 }
      }
    },
    "memory": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "raw_pdb",
        "stream_storage",
        "function_table",
        "argument_table",
        "global_table",
        "struct_table",
        "field_table",
        "string_arena",
        "total"
      ],
      "properties": {
        "raw_pdb":        { "type": "integer", "minimum": 0 },
        "stream_storage": { "type": "integer", "minimum": 0 },
        "function_table": { "type": "integer", "minimum": 0 },
        "argument_table": { "type": "integer", "minimum": 0 },
        "global_table":   { "type": "integer", "minimum": 0 },
        "struct_table":   { "type": "integer", "minimum": 0 },
        "field_table":    { "type": "integer", "minimum": 0 },
        "string_arena":   { "type": "integer", "minimum": 0 },
        "total":          { "type": "integer", "minimum": 0 }
      }
    }
  }
}
```

`memory.*` values are byte counts. `parse_ms.*` values are milliseconds.

### `index.json`

Manifest of files written for this run.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://mempdb.local/schemas/index.json",
  "title": "pdb2json index.json",
  "type": "object",
  "additionalProperties": false,
  "required": ["files"],
  "properties": {
    "files": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "description"],
        "properties": {
          "name":        { "type": "string" },
          "description": { "type": "string" },
          "records":     { "type": "integer", "minimum": 0 }
        }
      }
    }
  }
}
```

`records` is omitted for `metadata.json` in the manifest; data tables include it.
