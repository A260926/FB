# COMDAT Selection Analysis

@page selection COMDAT Selection Analysis

@tableofcontents

@section selection_scope Support scope

The selection analyzer is a read-only facility exported by the main
`coffi.hpp` header. It answers two questions about an ordered set of object
files:

- Which copy of each duplicated COMDAT section survives, and why were the
  others discarded?
- Which of the raw relocations still reference surviving content, and how do
  their symbols resolve under the selection?

It only accepts plain little-endian AMD64 COFF object files
(`IMAGE_FILE_MACHINE_AMD64`) as loaded by COFFI::coffi::load():

- no PE optional header (ordinary `.obj` files),
- no BigObj headers, no weak externals, no common symbols and no archives,
- PE architecture as represented by the library.

The analyzer never loads, executes, lays out or relocates anything, and it
never produces linked addresses. Symbol values stay section-relative and
sections keep their physical identity: a zero-based input module index plus
the one-based physical section number inside that module. Symbols keep their
module index and the raw slot of their primary record (auxiliary records
occupy the slots immediately following the primary).

The existing COFFI read interfaces are not modified, querying a report never
touches the source objects, and the returned COFFI::selection_report owns
every string and container it exposes. The source `coffi` objects may be
destroyed as soon as COFFI::analyze_coffi_selection() returns; reports remain
valid and are independently copyable and movable.

@section selection_usage Public API usage

```cpp
#include <coffi/coffi.hpp>

COFFI::coffi obj0, obj1, obj2;
// ... load() each object in the desired link order ...

std::vector<const COFFI::coffi*> inputs = {&obj0, &obj1, &obj2};
COFFI::selection_report report = COFFI::analyze_coffi_selection(inputs);

if (!report.ok()) {
    for (const COFFI::selection_diagnostic& d : report.get_diagnostics()) {
        // d.module, d.section (one-based, 0 when not applicable) and
        // d.slot (raw symbol slot, UINT32_MAX when not applicable) locate
        // every problem precisely.
    }
}

for (const COFFI::root_group& group : report.get_root_groups()) {
    // group.get_key(): root symbol name shared by the candidates
    // group.selection: raw auxiliary selection value
    // group.candidates: every candidate, in input order
    // group.winner / group.losers: the selection verdict
}

for (const COFFI::association_record& a : report.get_associations()) {
    // a.chain is the full in-module parent chain, immediate parent first.
}

for (const COFFI::reference_record& r : report.get_active_references()) {
    // Relocations carried by retained source sections.
}

for (const COFFI::reference_record& r : report.get_suppressed_references()) {
    // Relocations carried by discarded sections, kept verbatim with the
    // suppression reason, and excluded from active resolution.
}
```

@section selection_rules Selection rules

A COMDAT section definition is a symbol with storage class
`IMAGE_SYM_CLASS_STATIC`, type and value zero, a section number and name
matching the section, and exactly one auxiliary record. The raw selection
value and the associative parent come from that auxiliary record
(COFFI::auxiliary_symbol_record_5).

Plain (non-COMDAT) sections are always retained. Non-associative COMDAT
sections are roots:

- the root key is the name of the single external, value-zero symbol defined
  in the section; having zero or several such symbols is an error;
- candidates sharing a key must share one selection strategy, otherwise a
  strategy-conflict diagnostic is produced;
- `IMAGE_COMDAT_SELECT_ANY` keeps the first candidate in input order;
- `IMAGE_COMDAT_SELECT_LARGEST` keeps the candidate with the largest raw
  section data length; equal lengths keep the first candidate;
- every other selection strategy is reported as unsupported.

`IMAGE_COMDAT_SELECT_ASSOCIATIVE` sections only follow the parent named by
their auxiliary record inside the same module; multi-level chains are
followed, and an associative section survives only when the terminal parent
ultimately survives. Associative sections never replace same-named sections
across modules, and an association pointing at a plain section is an error.
Out-of-range parents, non-COMDAT parents and cycles all carry diagnostics
with module and section numbers.

@section selection_refs Reference resolution

Every raw relocation is listed with its source section, in-section offset,
type, raw symbol slot and the referenced symbol's original name, value and
section number. Relocations of retained sections go to the active list;
relocations of discarded sections go to a separate suppressed list with the
discard reason and never participate in active resolution.

- Static symbols resolve only inside their own module: to their section when
  it survives, otherwise `discarded_target`.
- External symbols, whether originally undefined or defined in a section,
  resolve by name against every surviving external definition: zero matches
  is `unresolved`, one is `resolved`, several is `ambiguous`. No candidate is
  picked on ambiguity, and definitions in discarded sections never appear.
- Symbols with section number `IMAGE_SYM_ABSOLUTE` (-1) are `absolute`.
- Relocations naming an auxiliary record or a non-existent slot are
  `invalid_slot`; any other unsupported symbol class is reported explicitly.

All ordering is stable: sections by module then section number, references by
source section then the original relocation ordinal, and external candidates
by module then raw slot. Changing the input order re-runs the selection
without affecting any previously produced report.
