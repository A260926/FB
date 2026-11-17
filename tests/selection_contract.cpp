// Contract tests for the read-only COMDAT selection analyzer.
// Inputs are synthesized in C++ through task_support/fixture.hpp; nothing is
// loaded from disk, executed or relocated.

#include <coffi/coffi.hpp>
#include "task_support/check.h"
#include "task_support/fixture.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using COFFI::analyze_coffi_selection;
using COFFI::association_record;
using COFFI::external_candidate;
using COFFI::reference_record;
using COFFI::reference_resolution;
using COFFI::root_group;
using COFFI::section_identity;
using COFFI::selection_diagnostic_code;
using COFFI::selection_discard_reason;
using COFFI::selection_report;
using COFFI::selection_section_kind;
using COFFI::selection_section_record;
using COFFI::selection_symbol_record;
using COFFI::symbol_identity;

namespace {

//-------------------------------------------------------------------------
sample::bytes make_aux(uint32_t length, uint16_t number, uint8_t selection)
{
    sample::bytes a(18, 0);
    sample::put(a, 0, length, 4);
    sample::put(a, 12, number, 2);
    sample::put(a, 14, selection, 1);
    return a;
}

//-------------------------------------------------------------------------
std::unique_ptr<COFFI::coffi> load_object(const sample::bytes& bytes)
{
    auto obj = std::make_unique<COFFI::coffi>();
    std::string storage(bytes.begin(), bytes.end());
    std::istringstream stream(storage, std::ios::binary);
    if (!obj->load(stream)) {
        throw std::runtime_error("failed to load synthesized COFF object");
    }
    return obj;
}

//-------------------------------------------------------------------------
const selection_section_record* find_section(const selection_report& report,
                                             int module, int section)
{
    for (const auto& s : report.get_sections()) {
        if (s.id.module == module && s.id.section == section) {
            return &s;
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------
const reference_record* find_reference(
    const std::vector<reference_record>& list,
    int module, int section, uint32_t reloc_index)
{
    for (const auto& r : list) {
        if (r.source.module == module && r.source.section == section &&
            r.relocation_index == reloc_index) {
            return &r;
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------
const root_group* find_group(const selection_report& report,
                             const std::string& key)
{
    for (const auto& g : report.get_root_groups()) {
        if (g.key == key) {
            return &g;
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------
const association_record* find_association(const selection_report& report,
                                           int module, int section)
{
    for (const auto& a : report.get_associations()) {
        if (a.section.module == module && a.section.section == section) {
            return &a;
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------
bool has_diagnostic(const selection_report& report,
                    selection_diagnostic_code code, int module, int section,
                    uint32_t slot)
{
    for (const auto& d : report.get_diagnostics()) {
        if (d.code == code && d.module == module &&
            d.section == section && d.slot == slot) {
            return true;
        }
    }
    return false;
}

//-------------------------------------------------------------------------
// One COMDAT .text section carrying one external root symbol, plus a plain
// .refs section.
sample::bytes simple_comdat_module(const std::string& root_name,
                                   uint32_t text_size, uint8_t selection)
{
    std::vector<sample::section> secs = {
        {".text", sample::bytes(text_size), selection, 0, {}},
        {".refs", sample::bytes(8), 0, 0, {}},
    };
    std::vector<sample::symbol> syms = {
        {".text", 0, 1, 3, {make_aux(text_size, 0, selection)}},
        {root_name, 0, 1, 2, {}},
        {".refs", 0, 2, 3, {make_aux(8, 0, 0)}},
    };
    return sample::object(secs, syms);
}

} // namespace

// =========================================================================
// selection group
// =========================================================================

//-------------------------------------------------------------------------
static int case_largest_tie_keeps_first()
{
    auto bytes_a = simple_comdat_module("entry", 8,
                                        IMAGE_COMDAT_SELECT_LARGEST);
    auto bytes_b = simple_comdat_module("entry", 8,
                                        IMAGE_COMDAT_SELECT_LARGEST);
    auto a = load_object(bytes_a);
    auto b = load_object(bytes_b);

    selection_report ab = analyze_coffi_selection({a.get(), b.get()});
    TASK_CHECK(ab.ok());
    const root_group* g = find_group(ab, "entry");
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->candidates.size() == 2);
    TASK_CHECK(g->winner == (section_identity{0, 1}));
    TASK_CHECK(g->losers.size() == 1);
    TASK_CHECK(g->losers[0] == (section_identity{1, 1}));
    const selection_section_record* lost = find_section(ab, 1, 1);
    TASK_CHECK(lost != nullptr);
    TASK_CHECK(lost->kept == false);
    TASK_CHECK(lost->discard_reason ==
               selection_discard_reason::lost_comdat);

    // Reversed input order flips the winner on an exact size tie.
    selection_report ba = analyze_coffi_selection({b.get(), a.get()});
    TASK_CHECK(ba.ok());
    const root_group* g2 = find_group(ba, "entry");
    TASK_CHECK(g2->winner == (section_identity{0, 1}));
    TASK_CHECK(g2->losers[0] == (section_identity{1, 1}));
    // The first report is independent of the second analysis.
    TASK_CHECK(find_group(ab, "entry")->winner == (section_identity{0, 1}));
    TASK_CHECK(find_section(ab, 0, 1)->kept == true);
    TASK_CHECK(find_section(ba, 1, 1)->kept == false);
    return 0;
}

//-------------------------------------------------------------------------
static int case_largest_picks_bigger()
{
    auto small = load_object(
        simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_LARGEST));
    auto big = load_object(
        simple_comdat_module("entry", 16, IMAGE_COMDAT_SELECT_LARGEST));

    selection_report r = analyze_coffi_selection({small.get(), big.get()});
    TASK_CHECK(r.ok());
    TASK_CHECK(find_group(r, "entry")->winner == (section_identity{1, 1}));

    // Order cannot save the smaller section.
    selection_report reversed =
        analyze_coffi_selection({big.get(), small.get()});
    TASK_CHECK(find_group(reversed, "entry")->winner ==
               (section_identity{0, 1}));
    TASK_CHECK(find_section(reversed, 1, 1)->kept == false);
    return 0;
}

//-------------------------------------------------------------------------
static int case_any_reorder_changes_winner()
{
    auto a = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_ANY));
    auto b = load_object(
        simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_ANY));

    selection_report ab = analyze_coffi_selection({a.get(), b.get()});
    TASK_CHECK(ab.ok());
    TASK_CHECK(find_group(ab, "entry")->winner == (section_identity{0, 1}));

    selection_report ba = analyze_coffi_selection({b.get(), a.get()});
    TASK_CHECK(find_group(ba, "entry")->winner == (section_identity{0, 1}));
    TASK_CHECK(find_section(ba, 1, 1)->discard_reason ==
               selection_discard_reason::lost_comdat);
    return 0;
}

//-------------------------------------------------------------------------
static int case_multilevel_association_chains()
{
    // Module 1 deliberately orders its sections differently (grandchild
    // .pdata first, root .text in the middle).
    auto m0 = load_object(sample::module(0));
    auto m1 = load_object(sample::module(1));
    auto m2 = load_object(sample::module(2));

    selection_report r = analyze_coffi_selection({m0.get(), m1.get(), m2.get()});
    TASK_CHECK(r.ok());

    // LARGEST: module 1 and 2 tie on size 12; first one (module 1) wins.
    const root_group* text = find_group(r, "entry");
    TASK_CHECK(text != nullptr);
    TASK_CHECK(text->winner == (section_identity{1, 2}));

    // Module 1 chain: .pdata(sec1) -> .xdata(sec3) -> .text(sec2).
    const association_record* pdata1 = find_association(r, 1, 1);
    TASK_CHECK(pdata1 != nullptr);
    TASK_CHECK(pdata1->parent_section == 3);
    TASK_CHECK(pdata1->kept == true);
    TASK_CHECK(pdata1->chain.size() == 2);
    TASK_CHECK(pdata1->chain[0] == (section_identity{1, 3}));
    TASK_CHECK(pdata1->chain[1] == (section_identity{1, 2}));

    const association_record* xdata1 = find_association(r, 1, 3);
    TASK_CHECK(xdata1 != nullptr);
    TASK_CHECK(xdata1->kept == true);
    TASK_CHECK(xdata1->chain.size() == 1);
    TASK_CHECK(xdata1->chain[0] == (section_identity{1, 2}));

    // Modules 0 and 2 lose their root, so their associative sections follow.
    const association_record* xdata0 = find_association(r, 0, 2);
    TASK_CHECK(xdata0 != nullptr);
    TASK_CHECK(xdata0->kept == false);
    TASK_CHECK(xdata0->chain.size() == 1);
    TASK_CHECK(xdata0->chain[0] == (section_identity{0, 1}));
    const selection_section_record* x0 = find_section(r, 0, 2);
    TASK_CHECK(x0->discard_reason ==
               selection_discard_reason::parent_discarded);

    const association_record* pdata0 = find_association(r, 0, 3);
    TASK_CHECK(pdata0 != nullptr);
    TASK_CHECK(pdata0->kept == false);
    TASK_CHECK(pdata0->chain.size() == 2);
    TASK_CHECK(pdata0->chain[0] == (section_identity{0, 2}));
    TASK_CHECK(pdata0->chain[1] == (section_identity{0, 1}));

    const association_record* pdata2 = find_association(r, 2, 3);
    TASK_CHECK(pdata2 != nullptr);
    TASK_CHECK(pdata2->kept == false);

    // Associations never replace same-named sections across modules: the
    // surviving .xdata is the one in the winning module only.
    TASK_CHECK(find_section(r, 1, 3)->kept == true);
    TASK_CHECK(find_section(r, 0, 2)->kept == false);
    TASK_CHECK(find_section(r, 2, 2)->kept == false);

    // Plain sections survive unconditionally.
    TASK_CHECK(find_section(r, 0, 4)->kept == true);
    TASK_CHECK(find_section(r, 1, 4)->kept == true);
    TASK_CHECK(find_section(r, 2, 4)->kept == true);
    return 0;
}

//-------------------------------------------------------------------------
static int case_unsupported_strategy_keeps_report_queryable()
{
    auto a = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_SAME_SIZE));
    auto b = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_SAME_SIZE));

    selection_report r = analyze_coffi_selection({a.get(), b.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::unsupported_selection,
                              0, 1, 0));
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::unsupported_selection,
                              1, 1, 0));

    // Normal queries still return complete, consistent data.
    TASK_CHECK(r.get_sections().size() == 4);
    const root_group* g = find_group(r, "entry");
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->supported == false);
    TASK_CHECK(g->winner.module == -1);
    TASK_CHECK(g->candidates.size() == 2);
    TASK_CHECK(g->losers.size() == 2);
    TASK_CHECK(find_section(r, 0, 1)->discard_reason ==
               selection_discard_reason::unsupported_strategy);
    TASK_CHECK(find_section(r, 1, 1)->discard_reason ==
               selection_discard_reason::unsupported_strategy);
    // The plain .refs sections are still retained.
    TASK_CHECK(find_section(r, 0, 2)->kept == true);
    return 0;
}

//-------------------------------------------------------------------------
static int case_strategy_conflict()
{
    auto any = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_ANY));
    auto largest = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_LARGEST));

    selection_report r = analyze_coffi_selection({any.get(), largest.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::strategy_conflict,
                              0, 1, 0));
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::strategy_conflict,
                              1, 1, 0));
    const root_group* g = find_group(r, "entry");
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->strategy_consistent == false);
    TASK_CHECK(g->supported == false);
    TASK_CHECK(g->winner.module == -1);
    return 0;
}

//-------------------------------------------------------------------------
static int case_missing_and_duplicate_section_definition()
{
    // COMDAT-flagged section without any section definition symbol.
    std::vector<sample::section> secs_a = {
        {".text", sample::bytes(4), IMAGE_COMDAT_SELECT_ANY, 0, {}},
    };
    std::vector<sample::symbol> syms_a = {
        {"entry", 0, 1, 2, {}},
    };
    auto obj_a = load_object(sample::object(secs_a, syms_a));

    // Two valid-looking section definition symbols for one section.
    std::vector<sample::section> secs_b = {
        {".text", sample::bytes(4), IMAGE_COMDAT_SELECT_ANY, 0, {}},
    };
    std::vector<sample::symbol> syms_b = {
        {".text", 0, 1, 3, {make_aux(4, 0, IMAGE_COMDAT_SELECT_ANY)}},
        {".text", 0, 1, 3, {make_aux(4, 0, IMAGE_COMDAT_SELECT_ANY)}},
        {"entry", 0, 1, 2, {}},
    };
    auto obj_b = load_object(sample::object(secs_b, syms_b));

    selection_report r =
        analyze_coffi_selection({obj_a.get(), obj_b.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::missing_section_definition,
                              0, 1, UINT32_MAX));
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::duplicate_section_definition,
                              1, 1, UINT32_MAX));
    TASK_CHECK(find_section(r, 0, 1)->kind ==
               selection_section_kind::invalid_comdat);
    TASK_CHECK(find_section(r, 1, 1)->kind ==
               selection_section_kind::invalid_comdat);
    TASK_CHECK(find_section(r, 0, 1)->kept == false);
    return 0;
}

//-------------------------------------------------------------------------
static int case_root_key_not_unique()
{
    std::vector<sample::section> secs = {
        {".text", sample::bytes(4), IMAGE_COMDAT_SELECT_ANY, 0, {}},
    };
    std::vector<sample::symbol> syms = {
        {".text", 0, 1, 3, {make_aux(4, 0, IMAGE_COMDAT_SELECT_ANY)}},
        {"entry", 0, 1, 2, {}},
        {"other", 0, 1, 2, {}},
    };
    auto obj = load_object(sample::object(secs, syms));
    selection_report r = analyze_coffi_selection({obj.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::root_key_not_unique,
                              0, 1, 0));
    TASK_CHECK(r.get_root_groups().empty());
    return 0;
}

//-------------------------------------------------------------------------
static sample::bytes associative_error_module(uint16_t parent_a,
                                              uint16_t parent_b)
{
    std::vector<sample::section> secs = {
        {".text", sample::bytes(4), IMAGE_COMDAT_SELECT_LARGEST, 0, {}},
        {".xdata", sample::bytes(4), IMAGE_COMDAT_SELECT_ASSOCIATIVE,
         parent_a, {}},
        {".pdata", sample::bytes(4), IMAGE_COMDAT_SELECT_ASSOCIATIVE,
         parent_b, {}},
        {".refs", sample::bytes(4), 0, 0, {}},
    };
    std::vector<sample::symbol> syms = {
        {".text", 0, 1, 3, {make_aux(4, 0, IMAGE_COMDAT_SELECT_LARGEST)}},
        {"entry", 0, 1, 2, {}},
        {".xdata", 0, 2, 3,
         {make_aux(4, parent_a, IMAGE_COMDAT_SELECT_ASSOCIATIVE)}},
        {".pdata", 0, 3, 3,
         {make_aux(4, parent_b, IMAGE_COMDAT_SELECT_ASSOCIATIVE)}},
        {".refs", 0, 4, 3, {make_aux(4, 0, 0)}},
    };
    return sample::object(secs, syms);
}

//-------------------------------------------------------------------------
static int case_association_errors()
{
    // Parent number outside the section table.
    auto out_of_range = load_object(associative_error_module(99, 1));
    selection_report r1 = analyze_coffi_selection({out_of_range.get()});
    TASK_CHECK(r1.ok() == false);
    TASK_CHECK(has_diagnostic(r1,
                              selection_diagnostic_code::associate_parent_out_of_range,
                              0, 2, UINT32_MAX));

    // Parent pointing at a plain (.refs) section.
    auto plain_parent = load_object(associative_error_module(4, 1));
    selection_report r2 = analyze_coffi_selection({plain_parent.get()});
    TASK_CHECK(r2.ok() == false);
    TASK_CHECK(has_diagnostic(r2,
                              selection_diagnostic_code::associate_parent_not_comdat,
                              0, 2, UINT32_MAX));
    // An association ending at a plain section is never retained, even
    // though the plain section itself survives.
    TASK_CHECK(find_section(r2, 0, 4)->kept == true);
    TASK_CHECK(find_section(r2, 0, 2)->kept == false);
    TASK_CHECK(find_section(r2, 0, 2)->discard_reason ==
               selection_discard_reason::invalid_definition);

    // A two-section associative cycle.
    std::vector<sample::section> secs = {
        {".a", sample::bytes(4), IMAGE_COMDAT_SELECT_ASSOCIATIVE, 2, {}},
        {".b", sample::bytes(4), IMAGE_COMDAT_SELECT_ASSOCIATIVE, 1, {}},
    };
    std::vector<sample::symbol> syms = {
        {".a", 0, 1, 3,
         {make_aux(4, 2, IMAGE_COMDAT_SELECT_ASSOCIATIVE)}},
        {".b", 0, 2, 3,
         {make_aux(4, 1, IMAGE_COMDAT_SELECT_ASSOCIATIVE)}},
    };
    auto cyclic = load_object(sample::object(secs, syms));
    selection_report r3 = analyze_coffi_selection({cyclic.get()});
    TASK_CHECK(r3.ok() == false);
    TASK_CHECK(has_diagnostic(r3,
                              selection_diagnostic_code::associate_cycle,
                              0, 1, UINT32_MAX) ||
               has_diagnostic(r3,
                              selection_diagnostic_code::associate_cycle,
                              0, 2, UINT32_MAX));
    TASK_CHECK(find_section(r3, 0, 1)->kept == false);
    TASK_CHECK(find_section(r3, 0, 2)->kept == false);
    return 0;
}

//-------------------------------------------------------------------------
static int case_null_and_duplicate_inputs()
{
    auto a = load_object(
        simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_ANY));

    selection_report r =
        analyze_coffi_selection({nullptr, a.get(), a.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r, selection_diagnostic_code::null_input,
                              0, 0, UINT32_MAX));
    TASK_CHECK(has_diagnostic(r, selection_diagnostic_code::duplicate_input,
                              2, 0, UINT32_MAX));
    // The valid module is still fully analyzed.
    TASK_CHECK(find_section(r, 1, 1) != nullptr);
    TASK_CHECK(find_section(r, 1, 1)->kept == true);
    return 0;
}

// =========================================================================
// references group
// =========================================================================

//-------------------------------------------------------------------------
static int case_baseline_reference_resolution()
{
    auto m0 = load_object(sample::module(0));
    auto m1 = load_object(sample::module(1));
    auto m2 = load_object(sample::module(2));

    selection_report r =
        analyze_coffi_selection({m0.get(), m1.get(), m2.get()});
    TASK_CHECK(r.ok());
    TASK_CHECK(r.get_active_references().size() == 10);
    TASK_CHECK(r.get_suppressed_references().size() == 2);

    // .text relocations: discarded roots are suppressed with their reason.
    const reference_record* suppressed0 =
        find_reference(r.get_suppressed_references(), 0, 1, 0);
    TASK_CHECK(suppressed0 != nullptr);
    TASK_CHECK(suppressed0->suppress_reason ==
               selection_discard_reason::lost_comdat);
    TASK_CHECK(suppressed0->symbol_name == "entry");
    const reference_record* suppressed2 =
        find_reference(r.get_suppressed_references(), 2, 1, 0);
    TASK_CHECK(suppressed2 != nullptr);
    TASK_CHECK(find_reference(r.get_active_references(), 0, 1, 0) ==
               nullptr);

    // The winning module's .text relocation is active.
    const reference_record* active_text =
        find_reference(r.get_active_references(), 1, 2, 0);
    TASK_CHECK(active_text != nullptr);
    TASK_CHECK(active_text->resolution == reference_resolution::resolved);
    TASK_CHECK(active_text->candidates.size() == 1);
    TASK_CHECK(active_text->candidates[0].symbol ==
               (symbol_identity{1, 4}));

    // Undefined "entry" relocations resolve only to the surviving definition.
    const reference_record* entry0 =
        find_reference(r.get_active_references(), 0, 4, 0);
    TASK_CHECK(entry0 != nullptr);
    TASK_CHECK(entry0->raw_slot == 11);
    TASK_CHECK(entry0->symbol_section == 0);
    TASK_CHECK(entry0->resolution == reference_resolution::resolved);
    TASK_CHECK(entry0->candidates.size() == 1);
    TASK_CHECK(entry0->candidates[0].section ==
               (section_identity{1, 2}));
    TASK_CHECK(entry0->candidates[0].value == 0);

    // Static "local": each module resolves only to its own symbol.
    const reference_record* local0 =
        find_reference(r.get_active_references(), 0, 4, 1);
    TASK_CHECK(local0 != nullptr);
    TASK_CHECK(local0->resolution ==
               reference_resolution::discarded_target);
    TASK_CHECK(local0->static_target == (symbol_identity{0, 5}));
    TASK_CHECK(local0->candidates.empty());

    const reference_record* local1 =
        find_reference(r.get_active_references(), 1, 4, 1);
    TASK_CHECK(local1 != nullptr);
    TASK_CHECK(local1->resolution == reference_resolution::resolved);
    TASK_CHECK(local1->static_target == (symbol_identity{1, 7}));
    TASK_CHECK(local1->static_target_section ==
               (section_identity{1, 3}));

    const reference_record* local2 =
        find_reference(r.get_active_references(), 2, 4, 1);
    TASK_CHECK(local2 != nullptr);
    TASK_CHECK(local2->resolution ==
               reference_resolution::discarded_target);

    // External "dup" defined in three retained plain sections: ambiguous,
    // candidates ordered by module then raw slot.
    const reference_record* dup0 =
        find_reference(r.get_active_references(), 0, 4, 2);
    TASK_CHECK(dup0 != nullptr);
    TASK_CHECK(dup0->resolution == reference_resolution::ambiguous);
    TASK_CHECK(dup0->candidates.size() == 3);
    TASK_CHECK(dup0->candidates[0].symbol == (symbol_identity{0, 10}));
    TASK_CHECK(dup0->candidates[1].symbol == (symbol_identity{1, 10}));
    TASK_CHECK(dup0->candidates[2].symbol == (symbol_identity{2, 10}));
    TASK_CHECK(dup0->candidates[0].section ==
               (section_identity{0, 4}));
    return 0;
}

//-------------------------------------------------------------------------
static int case_aux_and_missing_slots_are_located()
{
    std::vector<sample::section> secs = {
        {".text", sample::bytes(4), 0, 0,
         {{0, 1, 2}, {4, 7, 2}}},
    };
    std::vector<sample::symbol> syms = {
        {".text", 0, 1, 3, {make_aux(4, 0, 0)}}, // slot 0, aux at slot 1
        {"entry", 0, 0, 2, {}},                  // slot 2
    };
    auto obj = load_object(sample::object(secs, syms));
    selection_report r = analyze_coffi_selection({obj.get()});

    TASK_CHECK(r.ok() == false);
    const reference_record* aux_ref =
        find_reference(r.get_active_references(), 0, 1, 0);
    TASK_CHECK(aux_ref != nullptr);
    TASK_CHECK(aux_ref->raw_slot == 1);
    TASK_CHECK(aux_ref->resolution == reference_resolution::invalid_slot);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::invalid_relocation_slot,
                              0, 1, 1));

    const reference_record* missing_ref =
        find_reference(r.get_active_references(), 0, 1, 1);
    TASK_CHECK(missing_ref != nullptr);
    TASK_CHECK(missing_ref->raw_slot == 7);
    TASK_CHECK(missing_ref->resolution == reference_resolution::invalid_slot);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::invalid_relocation_slot,
                              0, 1, 7));
    return 0;
}

//-------------------------------------------------------------------------
static int case_absolute_and_unresolved_and_unsupported()
{
    std::vector<sample::section> secs = {
        // Slot 0 is .refs, slot 1 its auxiliary record: relocs must account
        // for the occupied aux slot.
        {".refs", sample::bytes(12), 0, 0,
         {{0, 2, 2}, {4, 3, 2}, {8, 4, 2}}},
    };
    std::vector<sample::symbol> syms = {
        {".refs", 0, 1, 3, {make_aux(12, 0, 0)}}, // slot 0, aux at slot 1
        {"abs_sym", 42, 0xFFFF, 2, {}},           // slot 2, section -1
        {"missing", 0, 0, 2, {}},                 // slot 3, undefined
        {"file_rec", 0, 0, 103, {}},              // slot 4, FILE class
    };
    auto obj = load_object(sample::object(secs, syms));
    selection_report r = analyze_coffi_selection({obj.get()});

    const reference_record* abs_ref =
        find_reference(r.get_active_references(), 0, 1, 0);
    TASK_CHECK(abs_ref != nullptr);
    TASK_CHECK(abs_ref->resolution == reference_resolution::absolute);
    TASK_CHECK(abs_ref->symbol_value == 42);

    const reference_record* unresolved =
        find_reference(r.get_active_references(), 0, 1, 1);
    TASK_CHECK(unresolved != nullptr);
    TASK_CHECK(unresolved->resolution ==
               reference_resolution::unresolved);
    TASK_CHECK(unresolved->candidates.empty());

    const reference_record* unsupported =
        find_reference(r.get_active_references(), 0, 1, 2);
    TASK_CHECK(unsupported != nullptr);
    TASK_CHECK(unsupported->resolution ==
               reference_resolution::unsupported_symbol);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::unsupported_symbol,
                              0, 1, 4));
    return 0;
}

//-------------------------------------------------------------------------
static int case_discarded_definition_leaves_candidate_set()
{
    // Both modules define "foo" inside ANY COMDAT sections; module 1 loses.
    auto m0 = load_object(
        simple_comdat_module("foo", 4, IMAGE_COMDAT_SELECT_ANY));
    auto m1 = load_object(
        simple_comdat_module("foo", 4, IMAGE_COMDAT_SELECT_ANY));

    std::vector<sample::section> secs = {
        {".refs", sample::bytes(4), 0, 0, {{0, 2, 2}}},
    };
    std::vector<sample::symbol> syms = {
        {".refs", 0, 1, 3, {make_aux(4, 0, 0)}},
        {"foo", 0, 0, 2, {}},
    };
    auto user = load_object(sample::object(secs, syms));

    selection_report r =
        analyze_coffi_selection({m0.get(), m1.get(), user.get()});
    TASK_CHECK(r.ok());
    const reference_record* ref =
        find_reference(r.get_active_references(), 2, 1, 0);
    TASK_CHECK(ref != nullptr);
    TASK_CHECK(ref->resolution == reference_resolution::resolved);
    TASK_CHECK(ref->candidates.size() == 1);
    TASK_CHECK(ref->candidates[0].section == (section_identity{0, 1}));
    return 0;
}

//-------------------------------------------------------------------------
static int case_static_same_names_are_module_local()
{
    auto build = [](unsigned filler) {
        std::vector<sample::section> secs = {
            {".data", sample::bytes(4), 0, 0, {}},
            // .refs occupies slot 2, its aux slot 3, so local is slot 4.
            {".refs", sample::bytes(4), 0, 0, {{0, 4, 2}}},
        };
        std::vector<sample::symbol> syms = {
            {".data", 0, 1, 3, {make_aux(4, 0, 0)}},
            {".refs", 0, 2, 3, {make_aux(4, 0, 0)}},
            {"local", filler, 1, 3, {}},
        };
        return sample::object(secs, syms);
    };
    auto a = load_object(build(7));
    auto b = load_object(build(9));

    selection_report r = analyze_coffi_selection({a.get(), b.get()});
    TASK_CHECK(r.ok());
    const reference_record* ra =
        find_reference(r.get_active_references(), 0, 2, 0);
    const reference_record* rb =
        find_reference(r.get_active_references(), 1, 2, 0);
    TASK_CHECK(ra != nullptr && rb != nullptr);
    TASK_CHECK(ra->resolution == reference_resolution::resolved);
    TASK_CHECK(rb->resolution == reference_resolution::resolved);
    TASK_CHECK(ra->static_target == (symbol_identity{0, 4}));
    TASK_CHECK(rb->static_target == (symbol_identity{1, 4}));
    TASK_CHECK(ra->symbol_value == 7);
    TASK_CHECK(rb->symbol_value == 9);
    TASK_CHECK(ra->candidates.empty());
    TASK_CHECK(rb->candidates.empty());
    return 0;
}

//-------------------------------------------------------------------------
static int case_suppressed_reference_preserves_raw_fields()
{
    // In the baseline modules module 0's .text loses LARGEST selection, so
    // its reloc must survive verbatim in the suppressed list.
    auto m0 = load_object(sample::module(0));
    auto m1 = load_object(sample::module(1));
    selection_report r2 = analyze_coffi_selection({m0.get(), m1.get()});
    const reference_record* s =
        find_reference(r2.get_suppressed_references(), 0, 1, 0);
    TASK_CHECK(s != nullptr);
    TASK_CHECK(s->source == (section_identity{0, 1}));
    TASK_CHECK(s->offset == 0);
    TASK_CHECK(s->type == 2);
    TASK_CHECK(s->raw_slot == 11);
    TASK_CHECK(s->symbol_name == "entry");
    TASK_CHECK(s->suppress_reason ==
               selection_discard_reason::lost_comdat);
    return 0;
}

// =========================================================================
// ownership group
// =========================================================================

//-------------------------------------------------------------------------
static int case_report_survives_destroyed_inputs()
{
    selection_report held;
    {
        auto a = load_object(
            simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_ANY));
        auto b = load_object(
            simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_ANY));
        selection_report local =
            analyze_coffi_selection({a.get(), b.get()});
        TASK_CHECK(local.ok());
        held = local; // Copy: the report owns all of its storage.
    }

    TASK_CHECK(held.get_module_count() == 2);
    TASK_CHECK(held.get_sections().size() == 4);
    const root_group* g = find_group(held, "entry");
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->winner == (section_identity{0, 1}));
    TASK_CHECK(held.get_sections()[0].name == ".text");
    TASK_CHECK(held.get_symbols().size() >= 4);

    // A moved-then-copied report is independently usable.
    selection_report moved = std::move(held);
    selection_report copied = moved;
    TASK_CHECK(find_group(copied, "entry")->winner ==
               (section_identity{0, 1}));
    TASK_CHECK(copied.get_sections().size() == 4);
    TASK_CHECK(moved.get_sections().size() == 4);
    return 0;
}

//-------------------------------------------------------------------------
static int case_reorder_does_not_pollute_old_report()
{
    // Distinct sizes so the same physical object lands at different module
    // indices between the two input orders.
    auto a = load_object(
        simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_LARGEST));
    auto b = load_object(
        simple_comdat_module("entry", 8, IMAGE_COMDAT_SELECT_LARGEST));

    selection_report first = analyze_coffi_selection({a.get(), b.get()});
    TASK_CHECK(first.ok());
    selection_report second = analyze_coffi_selection({b.get(), a.get()});

    // LARGEST always picks object b, but its module index moves with order.
    TASK_CHECK(find_group(first, "entry")->winner ==
               (section_identity{1, 1}));
    TASK_CHECK(find_group(second, "entry")->winner ==
               (section_identity{0, 1}));
    TASK_CHECK(find_group(first, "entry")->losers[0] ==
               (section_identity{0, 1}));
    TASK_CHECK(find_group(second, "entry")->losers[0] ==
               (section_identity{1, 1}));
    // Object a's section is module 0 in the first report, module 1 in the
    // second; the older report keeps its own copy untouched.
    TASK_CHECK(first.get_sections()[0].data_size == 4);
    TASK_CHECK(second.get_sections()[2].data_size == 4);
    return 0;
}

//-------------------------------------------------------------------------
static int case_input_bytes_and_order_are_stable()
{
    auto bytes0 = sample::module(0);
    auto bytes1 = sample::module(1);
    const sample::bytes saved0 = bytes0;
    const sample::bytes saved1 = bytes1;

    auto m0 = load_object(bytes0);
    auto m1 = load_object(bytes1);
    selection_report r = analyze_coffi_selection({m0.get(), m1.get()});
    TASK_CHECK(r.ok());

    // The analyzer never mutates the source byte vectors.
    TASK_CHECK(bytes0 == saved0);
    TASK_CHECK(bytes1 == saved1);

    // Sections are ordered by module then physical section number.
    const auto& secs = r.get_sections();
    TASK_CHECK(secs.size() == 8);
    for (size_t i = 1; i < secs.size(); ++i) {
        TASK_CHECK(secs[i - 1].id < secs[i].id);
    }

    // Active references are ordered by source section then reloc ordinal.
    const auto& refs = r.get_active_references();
    for (size_t i = 1; i < refs.size(); ++i) {
        const auto& x = refs[i - 1];
        const auto& y = refs[i];
        TASK_CHECK(x.source < y.source ||
                   (x.source == y.source &&
                    x.relocation_index < y.relocation_index));
    }

    // Ambiguous external candidates are ordered by module then raw slot.
    const reference_record* dup =
        find_reference(r.get_active_references(), 0, 4, 2);
    TASK_CHECK(dup != nullptr);
    for (size_t i = 1; i < dup->candidates.size(); ++i) {
        TASK_CHECK(dup->candidates[i - 1].symbol <
                   dup->candidates[i].symbol);
    }

    // Symbols carry the raw primary slot including aux coverage info.
    const selection_symbol_record* def = nullptr;
    for (const auto& s : r.get_symbols()) {
        if (s.id.module == 1 && s.id.slot == 2) {
            def = &s;
        }
    }
    TASK_CHECK(def != nullptr);
    TASK_CHECK(def->name == ".text");
    TASK_CHECK(def->auxiliary_count == 1);
    TASK_CHECK(def->section_number == 2);
    return 0;
}

//-------------------------------------------------------------------------
static int case_failed_analysis_remains_queryable()
{
    // Structural failure in module 0 must not suppress the rest of the report.
    std::vector<sample::section> bad_secs = {
        {".text", sample::bytes(4), IMAGE_COMDAT_SELECT_ANY, 0, {}},
    };
    auto bad = load_object(sample::object(bad_secs,
                                          std::vector<sample::symbol>{}));
    auto good = load_object(
        simple_comdat_module("entry", 4, IMAGE_COMDAT_SELECT_ANY));

    selection_report r = analyze_coffi_selection({bad.get(), good.get()});
    TASK_CHECK(r.ok() == false);
    TASK_CHECK(has_diagnostic(r,
                              selection_diagnostic_code::missing_section_definition,
                              0, 1, UINT32_MAX));
    TASK_CHECK(find_section(r, 1, 1) != nullptr);
    TASK_CHECK(find_section(r, 1, 1)->kept == true);
    const root_group* g = find_group(r, "entry");
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->winner == (section_identity{1, 1}));
    TASK_CHECK(r.get_symbols().size() >= 2);
    return 0;
}

// =========================================================================
// Registration (driver.c is compiled as C++ by task_support/run.sh).
// =========================================================================

const task_case task_cases[] = {
    // selection
    {"selection", "largest_tie_keeps_first", case_largest_tie_keeps_first},
    {"selection", "largest_picks_bigger", case_largest_picks_bigger},
    {"selection", "any_reorder_changes_winner",
     case_any_reorder_changes_winner},
    {"selection", "multilevel_association_chains",
     case_multilevel_association_chains},
    {"selection", "unsupported_strategy_keeps_report_queryable",
     case_unsupported_strategy_keeps_report_queryable},
    {"selection", "strategy_conflict", case_strategy_conflict},
    {"selection", "missing_and_duplicate_section_definition",
     case_missing_and_duplicate_section_definition},
    {"selection", "root_key_not_unique", case_root_key_not_unique},
    {"selection", "association_errors", case_association_errors},
    {"selection", "null_and_duplicate_inputs", case_null_and_duplicate_inputs},
    // references
    {"references", "baseline_reference_resolution",
     case_baseline_reference_resolution},
    {"references", "aux_and_missing_slots_are_located",
     case_aux_and_missing_slots_are_located},
    {"references", "absolute_unresolved_unsupported",
     case_absolute_and_unresolved_and_unsupported},
    {"references", "discarded_definition_leaves_candidate_set",
     case_discarded_definition_leaves_candidate_set},
    {"references", "static_same_names_are_module_local",
     case_static_same_names_are_module_local},
    {"references", "suppressed_reference_preserves_raw_fields",
     case_suppressed_reference_preserves_raw_fields},
    // ownership
    {"ownership", "report_survives_destroyed_inputs",
     case_report_survives_destroyed_inputs},
    {"ownership", "reorder_does_not_pollute_old_report",
     case_reorder_does_not_pollute_old_report},
    {"ownership", "input_bytes_and_order_are_stable",
     case_input_bytes_and_order_are_stable},
    {"ownership", "failed_analysis_remains_queryable",
     case_failed_analysis_remains_queryable},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
