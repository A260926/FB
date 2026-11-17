/*
Copyright (C) 2014-2014 by Serge Lamikhov-Center

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/*! @file coffi_selection.hpp
 * @brief Read-only COMDAT selection and reference analysis for ordered
 *        sets of AMD64 COFF object files.
 *
 * Do not include this file directly. It is included by coffi.hpp.
 *
 * @section scope Support scope
 *
 * The analyzer accepts an ordered list of already-loaded, read-only
 * COFFI::coffi objects. It only supports plain little-endian AMD64 COFF
 * object files:
 *   - machine #IMAGE_FILE_MACHINE_AMD64,
 *   - no PE optional header (ordinary .obj),
 *   - no BigObj headers, no weak externals, no common symbols,
 *     no archives (each list element is one object),
 *   - PE architecture as represented by COFFI.
 *
 * The analyzer never loads, executes, relocates or lays out anything.
 * It does not compute linked addresses: section-relative symbol values and
 * physical (module, one-based section number) identities are preserved as
 * found in the input bytes. The existing COFFI read interfaces are not
 * modified, and querying a report never touches the source objects.
 *
 * The returned selection_report owns every string and container it exposes.
 * The source coffi objects may be destroyed as soon as the analysis call
 * returns; reports remain valid and independently copyable.
 *
 * @section usage Public API usage
 * @code
 * std::vector<const COFFI::coffi*> inputs = {&obj0, &obj1, &obj2};
 * COFFI::selection_report report = COFFI::analyze_coffi_selection(inputs);
 * if (!report.ok()) {
 *     for (const auto& d : report.get_diagnostics()) {
 *         // every diagnostic carries module, physical section number
 *         // (when relevant) and the raw symbol table slot (when relevant).
 *     }
 * }
 * for (const auto& group : report.get_root_groups()) {
 *     // group.get_key(), raw selection value, candidates, winner, losers.
 * }
 * for (const auto& ref : report.get_active_references()) {
 *     // relocations whose source section survived selection.
 * }
 * for (const auto& ref : report.get_suppressed_references()) {
 *     // relocations carried by discarded sections, kept for the record.
 * }
 * @endcode
 *
 * @section rules Selection rules implemented
 *
 * A COMDAT section definition is a symbol with storage class
 * #IMAGE_SYM_CLASS_STATIC, type and value zero, section number and name
 * matching the section, and exactly one auxiliary record. The raw selection
 * value and the associative parent (auxiliary format 5 'number' field) come
 * from that record.
 *
 * Non-associative COMDAT sections are roots. The root key is the name of the
 * single external (storage class #IMAGE_SYM_CLASS_EXTERNAL) value-zero
 * symbol defined in the section. Candidates sharing a key must share one
 * selection strategy. #IMAGE_COMDAT_SELECT_ANY keeps the first candidate in
 * input order; #IMAGE_COMDAT_SELECT_LARGEST keeps the candidate with the
 * largest raw section data length, ties going to the first candidate. Every
 * other strategy is reported as unsupported.
 *
 * #IMAGE_COMDAT_SELECT_ASSOCIATIVE sections only follow the parent named by
 * their auxiliary record inside the same module. Multi-level chains are
 * followed; an associative section survives only if its parent ultimately
 * survives. Associative sections never replace sections by name across
 * modules, and an association pointing at a plain (non-COMDAT) section is an
 * error, not a root.
 */

#ifndef COFFI_SELECTION_HPP
#define COFFI_SELECTION_HPP

#include <cstring>
#include <cstdint>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <coffi/coffi_types.hpp>
#include <coffi/coffi_utils.hpp>
#include <coffi/coffi_section.hpp>
#include <coffi/coffi_headers.hpp>
#include <coffi/coffi_strings.hpp>
#include <coffi/coffi_symbols.hpp>
#include <coffi/coffi_directory.hpp>

namespace COFFI {

class coffi;

//-------------------------------------------------------------------------
//! Kind of a physical section, as classified by the analyzer.
enum class selection_section_kind : uint8_t {
    //! Ordinary section without IMAGE_SCN_LNK_COMDAT; always retained.
    plain,
    //! COMDAT section whose selection is not ASSOCIATIVE.
    comdat_root,
    //! COMDAT section with IMAGE_COMDAT_SELECT_ASSOCIATIVE.
    comdat_associative,
    //! Flagged COMDAT but its section definition symbol is missing or duplicated.
    invalid_comdat,
};

//-------------------------------------------------------------------------
//! Why a physical section was discarded.
enum class selection_discard_reason : uint8_t {
    //! The section was retained.
    none,
    //! Root COMDAT that lost the selection for its key.
    lost_comdat,
    //! Associative section whose parent chain ends in a discarded section.
    parent_discarded,
    //! The section itself has a structural COMDAT error.
    invalid_definition,
    //! Root group whose strategy is conflicting or unsupported.
    unsupported_strategy,
};

//-------------------------------------------------------------------------
//! Resolution of one raw relocation against the symbol table.
enum class reference_resolution : uint8_t {
    //! The raw slot names an auxiliary record or a non-existent slot.
    invalid_slot,
    //! Symbol section number is IMAGE_SYM_ABSOLUTE (-1).
    absolute,
    //! Exactly one surviving definition (external) or the static target's
    //! section survived.
    resolved,
    //! External name with no surviving definition.
    unresolved,
    //! External name with several surviving definitions; none was picked.
    ambiguous,
    //! Static symbol whose owning section was discarded.
    discarded_target,
    //! Storage class / section number combination outside the rules.
    unsupported_symbol,
};

//-------------------------------------------------------------------------
//! Machine-readable diagnostic codes.
enum class selection_diagnostic_code : uint8_t {
    null_input,
    duplicate_input,
    unsupported_format,
    missing_section_definition,
    duplicate_section_definition,
    root_key_not_unique,
    strategy_conflict,
    unsupported_selection,
    associate_parent_out_of_range,
    associate_parent_not_comdat,
    associate_cycle,
    invalid_relocation_slot,
    unsupported_symbol,
};

//-------------------------------------------------------------------------
//! Physical identity of a section: input module index (zero based) and
//! one-based physical section number inside that module.
struct section_identity
{
    int module  = -1;
    int section = 0;

    bool operator==(const section_identity& other) const
    {
        return module == other.module && section == other.section;
    }
    bool operator!=(const section_identity& other) const
    {
        return !(*this == other);
    }
    bool operator<(const section_identity& other) const
    {
        if (module != other.module) {
            return module < other.module;
        }
        return section < other.section;
    }
};

//-------------------------------------------------------------------------
//! Physical identity of a symbol: input module index (zero based) and the
//! raw symbol table slot of its primary record (auxiliary records occupy the
//! slots immediately following the primary).
struct symbol_identity
{
    int      module = -1;
    uint32_t slot   = 0;

    bool operator==(const symbol_identity& other) const
    {
        return module == other.module && slot == other.slot;
    }
    bool operator!=(const symbol_identity& other) const
    {
        return !(*this == other);
    }
    bool operator<(const symbol_identity& other) const
    {
        if (module != other.module) {
            return module < other.module;
        }
        return slot < other.slot;
    }
};

//-------------------------------------------------------------------------
//! One primary symbol record copied out of an input object.
struct selection_symbol_record
{
    symbol_identity id;
    std::string     name;
    uint32_t        value           = 0;
    int16_t         section_number  = 0;
    uint16_t        type            = 0;
    uint8_t         storage_class   = 0;
    uint8_t         auxiliary_count = 0;
};

//-------------------------------------------------------------------------
//! Every physical section of every input module.
struct selection_section_record
{
    section_identity          id;
    std::string               name;
    uint32_t                  data_size       = 0;
    uint32_t                  flags           = 0;
    selection_section_kind    kind            = selection_section_kind::plain;
    //! Raw auxiliary selection value; 0 for plain sections.
    uint8_t                   selection       = 0;
    //! One-based associative parent section number; 0 when not associative.
    int                       parent_section  = 0;
    //! Raw slot of the section-definition symbol, or -1 when missing.
    int32_t                   definition_slot = -1;
    bool                      kept            = false;
    selection_discard_reason  discard_reason  = selection_discard_reason::none;
    //! Root key for non-associative COMDAT sections; empty otherwise.
    std::string               root_key;
};

//-------------------------------------------------------------------------
//! One candidate inside a root COMDAT group.
struct root_candidate
{
    section_identity section;
    symbol_identity  definition;
    uint32_t         data_size = 0;
    uint8_t          selection = 0;
};

//-------------------------------------------------------------------------
//! A group of root COMDAT candidates sharing one key.
struct root_group
{
    std::string                 key;
    //! Raw selection value; meaningful only when strategy_consistent.
    uint8_t                     selection           = 0;
    bool                        strategy_consistent = true;
    //! False for any strategy other than ANY/LARGEST.
    bool                        supported           = true;
    //! Candidates in input (module, section) order.
    std::vector<root_candidate> candidates;
    //! Winning section; module == -1 when no winner exists.
    section_identity            winner{-1, 0};
    //! Every non-winning candidate identity, in input order.
    std::vector<section_identity> losers;
};

//-------------------------------------------------------------------------
//! Associative section with its full parent chain inside one module.
struct association_record
{
    section_identity                 section;
    //! Raw one-based parent number from the auxiliary record.
    int                              parent_section = 0;
    //! Immediate parent first, terminal root last; truncated on cycles.
    std::vector<section_identity>    chain;
    bool                             kept = false;
};

//-------------------------------------------------------------------------
//! An external definition candidate offered to a relocation.
struct external_candidate
{
    symbol_identity  symbol;
    section_identity section;
    std::string      name;
    //! Section-relative value from the symbol record; no linked address.
    uint32_t         value = 0;
};

//-------------------------------------------------------------------------
//! One raw relocation with its analysis.
struct reference_record
{
    section_identity           source;
    //! Zero-based ordinal of the relocation inside its source section.
    uint32_t                   relocation_index = 0;
    uint32_t                   offset           = 0;
    uint16_t                   type             = 0;
    //! Raw symbol table slot named by the relocation.
    uint32_t                   raw_slot         = 0;
    std::string                symbol_name;
    uint32_t                   symbol_value     = 0;
    //! Raw section number of the referenced symbol (-1 absolute, 0 undefined).
    int16_t                    symbol_section   = 0;
    reference_resolution       resolution       = reference_resolution::unresolved;
    //! External definition candidates, ordered by module then raw slot.
    std::vector<external_candidate> candidates;
    //! Target for a resolved static symbol.
    symbol_identity            static_target{-1, 0};
    section_identity          static_target_section{-1, 0};
    //! Why the source section was suppressed (suppressed list only).
    selection_discard_reason   suppress_reason  = selection_discard_reason::none;
};

//-------------------------------------------------------------------------
//! A structural or reference diagnostic. Fields that do not apply keep
//! module == -1, section == 0 or slot == UINT32_MAX.
struct selection_diagnostic
{
    selection_diagnostic_code code;
    int                       module  = -1;
    int                       section = 0;
    uint32_t                  slot    = UINT32_MAX;
    std::string               message;
};

//-------------------------------------------------------------------------
//! The independently owned result of analyze_coffi_selection().
class selection_report
{
  public:
    selection_report()                               = default;
    selection_report(const selection_report&)        = default;
    selection_report(selection_report&&) noexcept    = default;
    selection_report& operator=(const selection_report&) = default;
    selection_report& operator=(selection_report&&) noexcept = default;
    ~selection_report()                              = default;

    //! True when no diagnostic was produced.
    bool ok() const { return diagnostics_.empty(); }

    int get_module_count() const { return module_count_; }

    const std::vector<selection_diagnostic>& get_diagnostics() const
    {
        return diagnostics_;
    }
    //! All physical sections, ordered by module then physical section number.
    const std::vector<selection_section_record>& get_sections() const
    {
        return sections_;
    }
    //! All primary symbols, ordered by module then raw slot.
    const std::vector<selection_symbol_record>& get_symbols() const
    {
        return symbols_;
    }
    const std::vector<root_group>& get_root_groups() const
    {
        return root_groups_;
    }
    //! Associative sections, ordered by module then physical section number.
    const std::vector<association_record>& get_associations() const
    {
        return associations_;
    }
    //! Relocations whose source section was retained.
    const std::vector<reference_record>& get_active_references() const
    {
        return active_references_;
    }
    //! Relocations whose source section was discarded; never resolved.
    const std::vector<reference_record>& get_suppressed_references() const
    {
        return suppressed_references_;
    }

  private:
    friend selection_report analyze_coffi_selection(
        const std::vector<const coffi*>& inputs);

    int                                    module_count_ = 0;
    std::vector<selection_diagnostic>      diagnostics_;
    std::vector<selection_section_record>  sections_;
    std::vector<selection_symbol_record>   symbols_;
    std::vector<root_group>                root_groups_;
    std::vector<association_record>        associations_;
    std::vector<reference_record>          active_references_;
    std::vector<reference_record>          suppressed_references_;
};

namespace detail {

//-------------------------------------------------------------------------
struct module_snapshot
{
    int                                              module = 0;
    // One entry per physical section, index zero based (number is +1).
    std::vector<selection_section_record>           sections;
    std::vector<selection_symbol_record>            symbols;
    // Raw slot -> index into symbols, or -1 for auxiliary/missing slots.
    std::vector<int32_t>                            slot_to_symbol;
    uint32_t                                        raw_slot_count = 0;
};

//-------------------------------------------------------------------------
inline void add_diagnostic(std::vector<selection_diagnostic>& out,
                           selection_diagnostic_code code, int module,
                           int section, uint32_t slot,
                           const std::string& message)
{
    out.push_back(selection_diagnostic{code, module, section, slot, message});
}

//-------------------------------------------------------------------------
//! Copies every relevant record of one loaded object into owned storage.
inline bool snapshot_module(const coffi* obj, int module_index,
                            module_snapshot& snap,
                            std::vector<selection_diagnostic>& diagnostics)
{
    snap.module = module_index;

    if (obj->get_architecture() != COFFI_ARCHITECTURE_PE ||
        obj->get_header() == nullptr ||
        obj->get_optional_header() != nullptr ||
        obj->get_header()->get_machine() != IMAGE_FILE_MACHINE_AMD64) {
        add_diagnostic(diagnostics,
                       selection_diagnostic_code::unsupported_format,
                       module_index, 0, UINT32_MAX,
                       "only plain little-endian AMD64 COFF objects without "
                       "optional headers are supported");
        return false;
    }

    const coff_header* header = obj->get_header();
    snap.raw_slot_count       = header->get_symbols_count();
    snap.slot_to_symbol.assign(snap.raw_slot_count, -1);

    // --- Symbols, preserving raw slots including auxiliary coverage.
    const std::vector<symbol>* src_symbols = obj->get_symbols();
    if (src_symbols != nullptr) {
        for (const symbol& s : *src_symbols) {
            selection_symbol_record rec;
            rec.id.module       = module_index;
            rec.id.slot         = s.get_index();
            rec.name            = s.get_name();
            rec.value           = s.get_value();
            rec.section_number  = static_cast<int16_t>(s.get_section_number());
            rec.type            = s.get_type();
            rec.storage_class   = s.get_storage_class();
            rec.auxiliary_count = s.get_aux_symbols_number();
            snap.symbols.push_back(std::move(rec));

            if (s.get_index() < snap.raw_slot_count) {
                snap.slot_to_symbol[s.get_index()] =
                    static_cast<int32_t>(snap.symbols.size() - 1);
            }
        }
    }

    // --- Physical sections.
    const sections& src_sections = obj->get_sections();
    snap.sections.resize(src_sections.get_count());
    for (uint32_t i = 0; i < src_sections.get_count(); ++i) {
        const section* src = src_sections[i];
        selection_section_record& dst = snap.sections[i];
        dst.id.module  = module_index;
        dst.id.section = static_cast<int>(i) + 1;
        dst.name       = src->get_name();
        dst.data_size  = src->get_data_size();
        dst.flags      = src->get_flags();
        dst.kept       = (dst.flags & IMAGE_SCN_LNK_COMDAT) == 0;
        dst.kind       = dst.kept ? selection_section_kind::plain
                                  : selection_section_kind::invalid_comdat;
    }

    return true;
}

//-------------------------------------------------------------------------
//! Locates section definitions, root keys and associative parents.
inline void classify_sections(
    const coffi* obj, module_snapshot& snap,
    std::vector<selection_diagnostic>& diagnostics)
{
    const std::vector<symbol>* src_symbols = obj->get_symbols();
    if (src_symbols == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < snap.sections.size(); ++i) {
        selection_section_record& sec = snap.sections[i];
        if ((sec.flags & IMAGE_SCN_LNK_COMDAT) == 0) {
            continue; // Plain sections are unconditionally retained.
        }

        const uint16_t section_number = static_cast<uint16_t>(i + 1);
        const symbol*  definition     = nullptr;
        unsigned       definition_count = 0;

        for (const symbol& s : *src_symbols) {
            const bool is_definition =
                s.get_storage_class() == IMAGE_SYM_CLASS_STATIC &&
                s.get_type() == 0 && s.get_value() == 0 &&
                s.get_section_number() == section_number &&
                s.get_name() == sec.name &&
                s.get_aux_symbols_number() == 1;
            if (!is_definition) {
                continue;
            }
            ++definition_count;
            definition = &s;
        }

        if (definition_count == 0) {
            add_diagnostic(diagnostics,
                           selection_diagnostic_code::missing_section_definition,
                           snap.module, section_number, UINT32_MAX,
                           "COMDAT section has no section definition symbol");
            sec.kind           = selection_section_kind::invalid_comdat;
            sec.kept           = false;
            sec.discard_reason = selection_discard_reason::invalid_definition;
            continue;
        }
        if (definition_count > 1) {
            add_diagnostic(diagnostics,
                           selection_diagnostic_code::duplicate_section_definition,
                           snap.module, section_number, UINT32_MAX,
                           "COMDAT section has more than one section "
                           "definition symbol");
            sec.kind           = selection_section_kind::invalid_comdat;
            sec.kept           = false;
            sec.discard_reason = selection_discard_reason::invalid_definition;
            continue;
        }

        auxiliary_symbol_record_5 aux{};
        std::memcpy(&aux,
                    definition->get_auxiliary_symbols()[0].value,
                    sizeof(aux));

        sec.definition_slot = static_cast<int32_t>(definition->get_index());
        sec.selection       = aux.selection;
        sec.parent_section  = aux.number;

        if (aux.selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE) {
            sec.kind = selection_section_kind::comdat_associative;
            sec.kept = false;
        }
        else {
            sec.kind = selection_section_kind::comdat_root;
            sec.kept = false;

            // The root key is the unique external value-zero symbol defined
            // in this section.
            const symbol* key_symbol  = nullptr;
            unsigned      key_count   = 0;
            for (const symbol& s : *src_symbols) {
                if (s.get_storage_class() == IMAGE_SYM_CLASS_EXTERNAL &&
                    s.get_value() == 0 &&
                    s.get_section_number() == section_number) {
                    ++key_count;
                    key_symbol = &s;
                }
            }
            if (key_count != 1) {
                add_diagnostic(diagnostics,
                               selection_diagnostic_code::root_key_not_unique,
                               snap.module, section_number,
                               definition->get_index(),
                               "root COMDAT section must contain exactly one "
                               "value-zero external definition symbol");
                sec.kind           = selection_section_kind::invalid_comdat;
                sec.discard_reason = selection_discard_reason::invalid_definition;
            }
            else {
                sec.root_key = key_symbol->get_name();
            }
        }
    }
}

//-------------------------------------------------------------------------
//! Validates associative parent references, including cycle detection.
inline void validate_associations(
    module_snapshot& snap,
    std::vector<selection_diagnostic>& diagnostics)
{
    const int count = static_cast<int>(snap.sections.size());

    for (int i = 0; i < count; ++i) {
        selection_section_record& sec = snap.sections[i];
        if (sec.kind != selection_section_kind::comdat_associative) {
            continue;
        }
        const int parent = sec.parent_section;
        if (parent < 1 || parent > count) {
            add_diagnostic(diagnostics,
                           selection_diagnostic_code::associate_parent_out_of_range,
                           snap.module, i + 1, UINT32_MAX,
                           "associative parent section number is out of range");
            sec.discard_reason = selection_discard_reason::invalid_definition;
            continue;
        }
        const selection_section_record& parent_sec =
            snap.sections[parent - 1];
        if ((parent_sec.flags & IMAGE_SCN_LNK_COMDAT) == 0) {
            add_diagnostic(diagnostics,
                           selection_diagnostic_code::associate_parent_not_comdat,
                           snap.module, i + 1, UINT32_MAX,
                           "associative parent is a plain section, not a "
                           "COMDAT section");
            sec.discard_reason = selection_discard_reason::invalid_definition;
        }
    }

    // Cycle detection across associative edges within this module.
    std::vector<uint8_t> color(count, 0); // 0 white, 1 gray, 2 black
    for (int start = 0; start < count; ++start) {
        if (snap.sections[start].kind !=
                selection_section_kind::comdat_associative ||
            color[start] != 0) {
            continue;
        }
        std::vector<int> stack;
        stack.push_back(start);
        while (!stack.empty()) {
            int node = stack.back();
            if (color[node] == 0) {
                color[node] = 1;
            }
            const selection_section_record& node_sec = snap.sections[node];
            int parent = node_sec.parent_section;
            if (node_sec.kind !=
                    selection_section_kind::comdat_associative ||
                parent < 1 || parent > count) {
                color[node] = 2;
                stack.pop_back();
                continue;
            }
            int next = parent - 1;
            if (color[next] == 1) {
                add_diagnostic(diagnostics,
                               selection_diagnostic_code::associate_cycle,
                               snap.module, node + 1, UINT32_MAX,
                               "associative parent chain contains a cycle");
                // Every section participating in the cycle is invalid.
                auto cycle_start = std::find(stack.begin(), stack.end(), next);
                for (auto it = cycle_start; it != stack.end(); ++it) {
                    snap.sections[*it].discard_reason =
                        selection_discard_reason::invalid_definition;
                }
                color[node] = 2;
                stack.pop_back();
                continue;
            }
            if (color[next] == 0) {
                stack.push_back(next);
            }
            else {
                color[node] = 2;
                stack.pop_back();
            }
        }
    }
}

//-------------------------------------------------------------------------
inline bool section_kept(const std::vector<module_snapshot>& modules,
                         const section_identity& id)
{
    if (id.module < 0 ||
        id.module >= static_cast<int>(modules.size())) {
        return false;
    }
    const auto& secs = modules[id.module].sections;
    if (id.section < 1 ||
        id.section > static_cast<int>(secs.size())) {
        return false;
    }
    return secs[id.section - 1].kept;
}

//-------------------------------------------------------------------------
//! Builds root groups, checks strategy consistency and applies the
//! supported selection strategies.
inline void select_roots(
    std::vector<module_snapshot>& modules,
    std::vector<root_group>& groups,
    std::vector<selection_diagnostic>& diagnostics)
{
    std::map<std::string, size_t> group_index;

    for (module_snapshot& mod : modules) {
        for (selection_section_record& sec : mod.sections) {
            if (sec.kind != selection_section_kind::comdat_root ||
                sec.root_key.empty()) {
                continue;
            }
            auto found = group_index.find(sec.root_key);
            if (found == group_index.end()) {
                root_group group;
                group.key       = sec.root_key;
                group.selection = sec.selection;
                group_index.emplace(sec.root_key, groups.size());
                found = group_index.find(sec.root_key);
                groups.push_back(std::move(group));
            }
            root_group& group = groups[found->second];

            if (group.selection != sec.selection) {
                // Diagnostics are emitted per candidate once the whole group
                // is known; here the group is only marked inconsistent.
                group.strategy_consistent = false;
            }

            root_candidate candidate;
            candidate.section    = sec.id;
            candidate.definition = {mod.module,
                                    static_cast<uint32_t>(sec.definition_slot)};
            candidate.data_size  = sec.data_size;
            candidate.selection  = sec.selection;
            group.candidates.push_back(std::move(candidate));
        }
    }

    for (root_group& group : groups) {
        if (!group.strategy_consistent) {
            group.supported = false;
            group.winner    = section_identity{-1, 0};
            for (const root_candidate& cand : group.candidates) {
                group.losers.push_back(cand.section);
                add_diagnostic(diagnostics,
                               selection_diagnostic_code::strategy_conflict,
                               cand.section.module, cand.section.section,
                               cand.definition.slot,
                               "COMDAT candidates sharing key '" + group.key +
                                   "' use different selection strategies");
            }
            continue;
        }

        const uint8_t strategy = group.selection;
        if (strategy != IMAGE_COMDAT_SELECT_ANY &&
            strategy != IMAGE_COMDAT_SELECT_LARGEST) {
            group.supported = false;
            for (const root_candidate& cand : group.candidates) {
                group.losers.push_back(cand.section);
                add_diagnostic(diagnostics,
                               selection_diagnostic_code::unsupported_selection,
                               cand.section.module, cand.section.section,
                               cand.definition.slot,
                               "COMDAT selection strategy " +
                                   std::to_string(static_cast<unsigned>(strategy)) +
                                   " for key '" + group.key + "' is not supported");
            }
            continue;
        }

        size_t winner_pos = 0;
        if (strategy == IMAGE_COMDAT_SELECT_LARGEST) {
            for (size_t i = 1; i < group.candidates.size(); ++i) {
                // Strictly greater: equal length keeps the first candidate.
                if (group.candidates[i].data_size >
                    group.candidates[winner_pos].data_size) {
                    winner_pos = i;
                }
            }
        }
        group.winner = group.candidates[winner_pos].section;
        for (size_t i = 0; i < group.candidates.size(); ++i) {
            if (i != winner_pos) {
                group.losers.push_back(group.candidates[i].section);
            }
        }
    }

    // Apply the verdicts to the section records.
    for (const root_group& group : groups) {
        for (const root_candidate& cand : group.candidates) {
            selection_section_record& sec =
                modules[cand.section.module].sections[cand.section.section - 1];
            if (group.supported && group.winner == cand.section) {
                sec.kept           = true;
                sec.discard_reason = selection_discard_reason::none;
            }
            else {
                sec.kept           = false;
                sec.discard_reason =
                    group.supported
                        ? selection_discard_reason::lost_comdat
                        : selection_discard_reason::unsupported_strategy;
            }
        }
    }

    std::sort(groups.begin(), groups.end(),
              [](const root_group& a, const root_group& b) {
                  return a.key < b.key;
              });
}

//-------------------------------------------------------------------------
//! Resolves associative retention and records full parent chains.
inline void resolve_associations(
    std::vector<module_snapshot>& modules,
    std::vector<association_record>& associations)
{
    for (module_snapshot& mod : modules) {
        for (selection_section_record& sec : mod.sections) {
            if (sec.kind != selection_section_kind::comdat_associative) {
                continue;
            }

            association_record record;
            record.section        = sec.id;
            record.parent_section = sec.parent_section;

            // Walk the chain, guarding against cycles and bad parents.
            std::vector<section_identity> chain;
            std::vector<int>              visited;
            int current = sec.parent_section;
            const int guard = static_cast<int>(mod.sections.size()) + 1;
            for (int steps = 0; steps < guard; ++steps) {
                if (current < 1 ||
                    current > static_cast<int>(mod.sections.size())) {
                    current = -1;
                    break;
                }
                if (std::find(visited.begin(), visited.end(), current) !=
                    visited.end()) {
                    break; // Cycle: leave the chain truncated.
                }
                visited.push_back(current);
                const selection_section_record& node =
                    mod.sections[current - 1];
                chain.push_back(node.id);
                if (node.kind !=
                    selection_section_kind::comdat_associative) {
                    break;
                }
                current = node.parent_section;
            }
            record.chain = std::move(chain);

            bool kept = false;
            if (current >= 1 &&
                current <= static_cast<int>(mod.sections.size())) {
                const selection_section_record& terminal =
                    mod.sections[current - 1];
                kept = terminal.kept;
            }
            // A structurally invalid association (bad parent, cycle) is
            // never retained even if the walk reaches a retained section.
            if (sec.discard_reason ==
                selection_discard_reason::invalid_definition) {
                kept = false;
            }
            record.kept = kept;
            sec.kept    = kept;
            sec.discard_reason =
                kept ? selection_discard_reason::none
                     : (sec.discard_reason ==
                               selection_discard_reason::invalid_definition
                           ? selection_discard_reason::invalid_definition
                           : selection_discard_reason::parent_discarded);
            associations.push_back(std::move(record));
        }
    }
}

//-------------------------------------------------------------------------
struct external_definition
{
    symbol_identity  symbol;
    section_identity section;
    std::string      name;
    uint32_t         value = 0;
};

//-------------------------------------------------------------------------
//! Analyzes every raw relocation of every module.
inline void analyze_references(
    const std::vector<const coffi*>& inputs,
    std::vector<module_snapshot>& modules,
    std::vector<reference_record>& active,
    std::vector<reference_record>& suppressed,
    std::vector<selection_diagnostic>& diagnostics)
{
    // Index every external definition living in a retained section.
    std::map<std::string, std::vector<external_definition>> externals;
    for (const module_snapshot& mod : modules) {
        for (const selection_symbol_record& sym : mod.symbols) {
            if (sym.storage_class != IMAGE_SYM_CLASS_EXTERNAL ||
                sym.section_number <= 0) {
                continue;
            }
            const int secno = sym.section_number;
            if (secno < 1 || secno > static_cast<int>(mod.sections.size())) {
                continue;
            }
            if (!mod.sections[secno - 1].kept) {
                continue; // Discarded definitions leave the candidate set.
            }
            external_definition def;
            def.symbol  = sym.id;
            def.section = {mod.module, secno};
            def.name    = sym.name;
            def.value   = sym.value;
            externals[def.name].push_back(std::move(def));
        }
    }

    for (size_t m = 0; m < inputs.size(); ++m) {
        const coffi* obj = inputs[m];
        if (obj == nullptr) {
            continue; // Reported separately; nothing to read.
        }
        const module_snapshot& snap = modules[m];
        if (snap.sections.empty() &&
            obj->get_sections().get_count() != 0) {
            continue; // Module failed structural validation.
        }
        const sections& src_secs = obj->get_sections();

        for (uint32_t si = 0; si < src_secs.get_count(); ++si) {
            const section* src = src_secs[si];
            const selection_section_record& source_sec = snap.sections[si];
            const std::vector<relocation>& relocs = src->get_relocations();

            for (uint32_t ri = 0; ri < relocs.size(); ++ri) {
                const relocation& r = relocs[ri];

                reference_record ref;
                ref.source           = {static_cast<int>(m),
                                        static_cast<int>(si) + 1};
                ref.relocation_index = ri;
                ref.offset           = r.get_virtual_address();
                ref.type             = static_cast<uint16_t>(r.get_type());
                ref.raw_slot         = r.get_symbol_table_index();
                ref.suppress_reason  = source_sec.discard_reason;

                const bool slot_in_range =
                    ref.raw_slot < snap.raw_slot_count;
                const int32_t sym_index =
                    slot_in_range ? snap.slot_to_symbol[ref.raw_slot] : -1;

                if (sym_index < 0) {
                    ref.symbol_name  = r.get_symbol();
                    ref.resolution   = reference_resolution::invalid_slot;
                    add_diagnostic(diagnostics,
                                   selection_diagnostic_code::invalid_relocation_slot,
                                   static_cast<int>(m),
                                   static_cast<int>(si) + 1, ref.raw_slot,
                                   "relocation names an auxiliary or "
                                   "non-existent symbol slot");
                }
                else {
                    const selection_symbol_record& sym =
                        snap.symbols[static_cast<size_t>(sym_index)];
                    ref.symbol_name    = sym.name;
                    ref.symbol_value   = sym.value;
                    ref.symbol_section = sym.section_number;

                    if (sym.section_number == IMAGE_SYM_ABSOLUTE) {
                        ref.resolution = reference_resolution::absolute;
                    }
                    else if (sym.storage_class == IMAGE_SYM_CLASS_STATIC) {
                        // Static symbols resolve only inside their own module.
                        const int secno = sym.section_number;
                        if (secno >= 1 &&
                            secno <= static_cast<int>(snap.sections.size())) {
                            const selection_section_record& target =
                                snap.sections[secno - 1];
                            ref.static_target         = sym.id;
                            ref.static_target_section = {static_cast<int>(m),
                                                         secno};
                            ref.resolution =
                                target.kept
                                    ? reference_resolution::resolved
                                    : reference_resolution::discarded_target;
                        }
                        else {
                            ref.resolution =
                                reference_resolution::unsupported_symbol;
                            add_diagnostic(diagnostics,
                                           selection_diagnostic_code::unsupported_symbol,
                                           static_cast<int>(m),
                                           static_cast<int>(si) + 1,
                                           sym.id.slot,
                                           "static symbol has no valid "
                                           "section number");
                        }
                    }
                    else if (sym.storage_class == IMAGE_SYM_CLASS_EXTERNAL) {
                        // Defined or undefined externals alike resolve by name
                        // against surviving external definitions.
                        auto found = externals.find(sym.name);
                        if (found == externals.end()) {
                            ref.resolution = reference_resolution::unresolved;
                        }
                        else {
                            for (const external_definition& def : found->second) {
                                external_candidate cand;
                                cand.symbol  = def.symbol;
                                cand.section = def.section;
                                cand.name    = def.name;
                                cand.value   = def.value;
                                ref.candidates.push_back(std::move(cand));
                            }
                            if (ref.candidates.size() == 1) {
                                ref.resolution = reference_resolution::resolved;
                            }
                            else {
                                ref.resolution = reference_resolution::ambiguous;
                            }
                        }
                    }
                    else {
                        ref.resolution =
                            reference_resolution::unsupported_symbol;
                        add_diagnostic(diagnostics,
                                       selection_diagnostic_code::unsupported_symbol,
                                       static_cast<int>(m),
                                       static_cast<int>(si) + 1, sym.id.slot,
                                       "symbol storage class is outside the "
                                       "supported set");
                    }
                }

                if (source_sec.kept) {
                    active.push_back(std::move(ref));
                }
                else {
                    suppressed.push_back(std::move(ref));
                }
            }
        }
    }
}

} // namespace detail

//-------------------------------------------------------------------------
/*! @brief Analyzes COMDAT selection and raw references over an ordered
 *         list of loaded AMD64 COFF objects.
 *
 * @param[in] inputs Ordered, non-null, non-duplicated coffi pointers.
 *                   Input order is the link order used by ANY and by
 *                   LARGEST tie breaking.
 * @return A selection_report that owns all of its data. The report remains
 *         valid after the input objects are destroyed.
 */
inline selection_report
analyze_coffi_selection(const std::vector<const coffi*>& inputs)
{
    selection_report report;
    report.module_count_ = static_cast<int>(inputs.size());

    {
        std::vector<const coffi*> seen;
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i] == nullptr) {
                detail::add_diagnostic(
                    report.diagnostics_,
                    selection_diagnostic_code::null_input,
                    static_cast<int>(i), 0, UINT32_MAX,
                    "input list contains a null coffi pointer");
                continue;
            }
            if (std::find(seen.begin(), seen.end(), inputs[i]) != seen.end()) {
                detail::add_diagnostic(
                    report.diagnostics_,
                    selection_diagnostic_code::duplicate_input,
                    static_cast<int>(i), 0, UINT32_MAX,
                    "the same coffi object appears more than once");
                continue;
            }
            seen.push_back(inputs[i]);
        }
    }

    std::vector<detail::module_snapshot> modules;
    modules.reserve(inputs.size());

    for (size_t i = 0; i < inputs.size(); ++i) {
        detail::module_snapshot snap;
        if (inputs[i] != nullptr &&
            detail::snapshot_module(inputs[i], static_cast<int>(i), snap,
                                    report.diagnostics_)) {
            detail::classify_sections(inputs[i], snap, report.diagnostics_);
            detail::validate_associations(snap, report.diagnostics_);
        }
        modules.push_back(std::move(snap));
    }

    detail::select_roots(modules, report.root_groups_,
                         report.diagnostics_);
    detail::resolve_associations(modules, report.associations_);

    // Flatten owned section and symbol tables in stable (module, number) order.
    for (const detail::module_snapshot& snap : modules) {
        for (const auto& sec : snap.sections) {
            report.sections_.push_back(sec);
        }
        for (const auto& sym : snap.symbols) {
            report.symbols_.push_back(sym);
        }
    }

    // References: modules that failed structural validation are skipped
    // inside analyze_references, keeping module indices aligned.
    detail::analyze_references(
        inputs, modules, report.active_references_,
        report.suppressed_references_, report.diagnostics_);

    return report;
}

} // namespace COFFI

#endif // COFFI_SELECTION_HPP
