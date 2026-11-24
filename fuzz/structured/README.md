# Structured cgltf defense generator

This directory contains the always-legal structured path used by the
`cgltf_defense_main` entry point.  It intentionally does **not** generate
malformed byte streams.  Every generated candidate is a complete glTF 2.0
resource with embedded `data:` buffers, so defense runs reach
`cgltf_parse_file`, `cgltf_load_buffers`, `cgltf_validate`, and accessor
reading instead of stopping at JSON syntax handling.

## Commands

The public C entry point is:

```cpp
int cgltf_defense_main(int argc, char **argv);
```

`argv[0]` must be the executable used to spawn the matching `worker` process.

```text
<program> generate SEED DIR
<program> check DIR
<program> worker DIR
<program> reduce IN OUT --keep shared-strided --budget N [--resume]
```

`SEED` is a decimal `uint64_t`.  A fixed seed produces byte-identical
`case.plan` and `case.gltf` in independent processes.

## Plan format

`DIR/case.plan` is the independent semantic model.  It is line-oriented and is
not a copy of the glTF JSON.  The current format starts with:

```text
CGLTF-STRUCTURED-PLAN 1
SEED 7
```

Records use stable object IDs and `key=value` fields:

- `SCENE id=... roots=node-0001,...`
- `NODE id=node-0001 mesh=mesh-0001 children=node-0002,...`
- `MESH id=mesh-0001`
- `PRIM id=prim-0001 mesh=mesh-0001 mode=4 indices=acc-... position=acc-...`
- `TRI prim=prim-0001 id=tri-0001 a=vert-... b=vert-... c=vert-...`
- `ACCESSOR id=acc-0001 role=POSITION|INDEX componentType=5126|5123 type=VEC3|SCALAR view=view-0001 offset=12 explicitStride=24`
- `VERTEX accessor=acc-0001 id=vert-0001 x=... y=... z=...`
- `INDEXES accessor=acc-... values=0,1,2,...`
- `VIEW id=view-0001 buffer=buf-0001 offset=... size=... stride=24 target=34962`
- `BUFFER id=buf-0001 length=...`

Triangles reference stable vertex IDs, not compact glTF indices.  During layout
rebuild those references are converted to compact `uint16` indices.  Surviving
graph/resource/element IDs are never renumbered; only glTF array positions and
binary offsets are compacted.

The plan stores identities and numeric geometry independently.  Buffer bytes
and glTF JSON are derived layout/serialization products.

## Required coverage

Every generated seed contains:

- two scene-used primitives sharing one `POSITION` accessor;
- that accessor with nonzero `byteOffset` and a legal interleaved stride
  (`offset=12`, `byteStride=24` in the standard witness);
- multiple meshes and acyclic nodes;
- shared and non-shared accessors;
- out-of-scene objects and unused resources;
- no extensions and no external buffer URIs;
- at most 64 nodes, 64 accessors, and 64 KiB of buffer data.

## Worker protocol

`worker DIR` uses the production cgltf C API and prints a complete normalized
report beginning with `CGLTF_STRUCTURED_WORKER 1` and ending with `END OK`.
The report contains object names/array identities, buffer/view/accessor
layout, primitive references, node/scene references, sharing relations, and
every index/POSITION value.  `check DIR` spawns this worker in a new POSIX
process and compares the complete exit status and report against `case.plan`.

## Reduction state

`OUT/reduction.log` records the full candidate order and every
`OBSERVE`, `ACCEPT`, or `REJECT` result.  `OUT/reduction.state` stores the
source fingerprints, requested keep constraint, and next candidate cursor.
Each budget invocation observes at most `N` new candidates; every observed
candidate, including a rejection, consumes budget.  `--resume` continues at the
persisted cursor and rejects a changed input or mismatched keep constraint.
