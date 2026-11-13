# import_binding_plan

An offline example that builds a read-only import binding plan for a root PE
module and a caller-supplied set of provider modules.

```
import_binding_plan [--apiset <namespace.bin>] \
                    <root.dll> <provider.dll> [provider.dll ...]
```

* Every module comes from this list. The tool never loads a DLL, never runs an
  entry point, and never consults the system directory, the registry, or any
  other host file beyond the paths named on the command line.
* Module file names are reduced to their basename, ASCII case-folded, and an
  optional `.dll` suffix is stripped. Duplicate normalized names are rejected.
* Without `--apiset`, ordinary DLL imports still resolve; contract
  (`api-*`/`ext-*`) imports report a missing schema.

The output is JSON with one row per ordinary import of the root, stable-sorted
by the root's original IAT RVA. Each row carries the full request (module,
symbol or ordinal, IAT RVA/VA, slot width), the ordered hops (export forwards
with slot/ordinal/forwarder string, or API-set contract hops with schema
offsets), and the final provider/slot/RVA/VA. Failures report a specific
status plus a diagnostic; one failed import never erases the others.

Build it in-tree (`-DPEPARSE_ENABLE_EXAMPLES=ON`) or standalone against an
installed pe-parse:

```
cmake -S examples/import_binding_plan -B build
cmake --build build
```
