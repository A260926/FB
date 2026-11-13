/* depex.cpp

Copyright (c) 2026, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#include "depex.h"

#include <cstring>
#include <map>
#include <vector>
#include <algorithm>

#include "ffs.h"
#include "types.h"

namespace Depex {

// --------------------------------------------------------------------------
// Strict decoder
// --------------------------------------------------------------------------

bool decode(const UByteArray &body, DecodedDepex &out, DecodeError &error)
{
    out = DecodedDepex();
    out.body = body; // The decoded result owns its own copy of the bytes

    const UINT32 size = (UINT32)body.size();
    UINT32 pos = 0;

    if (size == 0) {
        error.offset = 0;
        error.message = UString("DEPEX body is empty");
        return false;
    }

    // Optional leading SOR opcode, legal only at offset 0
    if ((UINT8)body[0] == EFI_DEP_SOR) {
        out.sor = true;
        pos = 1;
    }

    UINT32 stackDepth = 0;
    bool ended = false;

    while (pos < size) {
        const UINT32 opOffset = pos;
        const UINT8 opcode = (UINT8)body[pos];

        if (opcode == EFI_DEP_BEFORE || opcode == EFI_DEP_AFTER) {
            error.offset = opOffset;
            error.message = usprintf("BEFORE/AFTER dependency opcode %02Xh is not supported", opcode);
            return false;
        }
        if (opcode == EFI_DEP_SOR) {
            error.offset = opOffset;
            error.message = UString("misplaced SOR opcode");
            return false;
        }

        switch (opcode) {
            case EFI_DEP_PUSH: {
                if (size - opOffset < EFI_DEP_OPCODE_SIZE + sizeof(EFI_GUID)) {
                    error.offset = opOffset;
                    error.message = UString("truncated PUSH opcode, 16 GUID bytes are required");
                    return false;
                }
                Operation op;
                op.opcode = opcode;
                op.kind = OpPush;
                op.offset = opOffset;
                memcpy(&op.guid, body.constData() + pos + EFI_DEP_OPCODE_SIZE, sizeof(EFI_GUID));
                out.operations.push_back(op);
                stackDepth++;
                pos += EFI_DEP_OPCODE_SIZE + (UINT32)sizeof(EFI_GUID);
                break;
            }
            case EFI_DEP_AND:
            case EFI_DEP_OR:
                if (stackDepth < 2) {
                    error.offset = opOffset;
                    error.message = UString("boolean opcode underflows the expression stack");
                    return false;
                }
                stackDepth--;
                {
                    Operation op;
                    op.opcode = opcode;
                    op.kind = (opcode == EFI_DEP_AND) ? OpAnd : OpOr;
                    op.offset = opOffset;
                    memset(&op.guid, 0, sizeof(op.guid));
                    out.operations.push_back(op);
                }
                pos += EFI_DEP_OPCODE_SIZE;
                break;
            case EFI_DEP_NOT:
                if (stackDepth < 1) {
                    error.offset = opOffset;
                    error.message = UString("NOT opcode underflows the expression stack");
                    return false;
                }
                {
                    Operation op;
                    op.opcode = opcode;
                    op.kind = OpNot;
                    op.offset = opOffset;
                    memset(&op.guid, 0, sizeof(op.guid));
                    out.operations.push_back(op);
                }
                pos += EFI_DEP_OPCODE_SIZE;
                break;
            case EFI_DEP_TRUE:
            case EFI_DEP_FALSE: {
                Operation op;
                op.opcode = opcode;
                op.kind = (opcode == EFI_DEP_TRUE) ? OpTrue : OpFalse;
                op.offset = opOffset;
                memset(&op.guid, 0, sizeof(op.guid));
                out.operations.push_back(op);
                stackDepth++;
                pos += EFI_DEP_OPCODE_SIZE;
                break;
            }
            case EFI_DEP_END:
                if (stackDepth != 1) {
                    error.offset = opOffset;
                    error.message = usprintf("END opcode leaves %u value(s) on the expression stack, exactly one is required", stackDepth);
                    return false;
                }
                ended = true;
                pos += EFI_DEP_OPCODE_SIZE;
                if (pos < size) {
                    error.offset = pos;
                    error.message = UString("trailing bytes after the END opcode");
                    return false;
                }
                break;
            default:
                error.offset = opOffset;
                error.message = usprintf("unknown DEPEX opcode %02Xh", opcode);
                return false;
        }
    }

    if (!ended) {
        error.offset = size;
        error.message = UString("DEPEX body ends without the END opcode");
        return false;
    }

    return true;
}

UString renderText(const DecodedDepex &decoded)
{
    UString parsed;
    if (decoded.sor)
        parsed += "\nSOR";

    for (size_t i = 0; i < decoded.operations.size(); i++) {
        const Operation &op = decoded.operations[i];
        switch (op.kind) {
            case OpPush:  parsed += UString("\nPUSH ") + guidToUString(op.guid, false); break;
            case OpAnd:   parsed += "\nAND"; break;
            case OpOr:    parsed += "\nOR"; break;
            case OpNot:   parsed += "\nNOT"; break;
            case OpTrue:  parsed += "\nTRUE"; break;
            case OpFalse: parsed += "\nFALSE"; break;
        }
    }
    // A successfully decoded stream always terminates with END.
    parsed += "\nEND";
    return parsed;
}

bool evaluate(const DecodedDepex &decoded, const GuidSet &installed)
{
    std::vector<bool> stack;
    stack.reserve(decoded.operations.size());

    for (size_t i = 0; i < decoded.operations.size(); i++) {
        const Operation &op = decoded.operations[i];
        switch (op.kind) {
            case OpPush:
                stack.push_back(installed.find(op.guid) != installed.end());
                break;
            case OpTrue:
                stack.push_back(true);
                break;
            case OpFalse:
                stack.push_back(false);
                break;
            case OpNot:
                // A strictly decoded expression can never underflow here.
                stack.back() = !stack.back();
                break;
            case OpAnd: {
                bool rhs = stack.back();
                stack.pop_back();
                stack.back() = stack.back() && rhs;
                break;
            }
            case OpOr: {
                bool rhs = stack.back();
                stack.pop_back();
                stack.back() = stack.back() || rhs;
                break;
            }
        }
    }

    // END is not represented as an operation; a valid stream leaves one value.
    return stack.empty() ? false : stack.back();
}

// --------------------------------------------------------------------------
// Node identity
// --------------------------------------------------------------------------

static const char *itemTypeName(UINT8 type)
{
    switch (type) {
        case Types::Root:      return "Root";
        case Types::Capsule:   return "Capsule";
        case Types::Image:     return "Image";
        case Types::Region:    return "Region";
        case Types::Padding:   return "Padding";
        case Types::Volume:    return "Volume";
        case Types::File:      return "File";
        case Types::Section:   return "Section";
        case Types::FreeSpace: return "FreeSpace";
        default:               return "Item";
    }
}

NodeId nodeIdOf(const UModelIndex &volume, const UModelIndex &node)
{
    struct Entry { UINT8 type; UINT32 offset; };
    std::vector<Entry> chain;

    UModelIndex current = node;
    while (current.isValid() && !(current == volume)) {
        Entry e;
        e.type = current.model()->type(current);
        e.offset = current.model()->offset(current);
        chain.push_back(e);
        current = current.model()->parent(current);
    }

    UString path;
    for (std::vector<Entry>::reverse_iterator it = chain.rbegin(); it != chain.rend(); ++it) {
        path += usprintf("/%s@%u", itemTypeName(it->type), it->offset);
    }
    if (path.isEmpty())
        path = UString("/");

    UINT32 base = 0;
    if (node.isValid())
        base = node.model()->base(node) - (volume.isValid() ? volume.model()->base(volume) : 0);

    return NodeId(path, base);
}

UString NodeId::toString() const
{
    // Concatenation keeps this identical under the Qt and the Bstrlib UString.
    return UString(path) + usprintf("+0x%X", base);
}

// --------------------------------------------------------------------------
// DEPEX extraction from a driver subtree
// --------------------------------------------------------------------------

static bool belongsToVolume(const UModelIndex &volume, const UModelIndex &node)
{
    UModelIndex current = node;
    while (current.isValid()) {
        if (current == volume)
            return true;
        current = current.model()->parent(current);
    }
    return false;
}

// Recursively collect every DXE_DEPEX section. Compression and GUID defined
// sections only count as traversable when the parser actually expanded them
// into child nodes.
static bool collectDepex(const UModelIndex &volume, const UModelIndex &index, const NodeId &fileId,
                         std::vector<std::pair<NodeId, UByteArray> > &found,
                         AnalysisReport &report)
{
    const TreeModel *model = index.model();
    int rows = model->rowCount(index);

    for (int row = 0; row < rows; row++) {
        UModelIndex child = model->index(row, 0, index);

        if (model->type(child) == Types::File || model->type(child) == Types::Volume) {
            // Nested files or firmware volumes are separate identity domains
            // and never contribute to this driver's DEPEX.
            continue;
        }

        if (model->type(child) == Types::Section) {
            UINT8 subtype = model->subtype(child);
            if (subtype == EFI_SECTION_PEI_DEPEX || subtype == EFI_SECTION_MM_DEPEX) {
                report.errorHasOffset = false;
                report.errorIdentity = fileId.toString();
                report.errorMessage = usprintf("%s dependency section is not supported, only DXE_DEPEX is analyzed",
                                               subtype == EFI_SECTION_PEI_DEPEX ? "PEI" : "MM");
                return false;
            }
            if ((subtype == EFI_SECTION_COMPRESSION || subtype == EFI_SECTION_GUID_DEFINED)
                && model->rowCount(child) == 0) {
                report.errorHasOffset = false;
                report.errorIdentity = fileId.toString();
                report.errorMessage = UString("unexpanded compression or GUID defined encapsulation cannot be analyzed");
                return false;
            }
            if (subtype == EFI_SECTION_DXE_DEPEX)
                found.push_back(std::make_pair(nodeIdOf(volume, child), model->body(child)));
        }

        if (!collectDepex(volume, child, fileId, found, report))
            return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// State exploration
// --------------------------------------------------------------------------

static const size_t MAX_DRIVERS = 18;

struct PreparedDriver {
    NodeId                 identity;
    DecodedDepex           depex;
    GuidSet                published;
    std::vector<EFI_GUID>  publishedList;
    bool                   sor;
};

static void fail(AnalysisReport &report, const UString &message, const NodeId &id, bool hasOffset, UINT32 offset)
{
    report.errorMessage = message;
    report.errorIdentity = id.toString();
    report.errorHasOffset = hasOffset;
    report.errorOffset = offset;
}

static void failNoIdentity(AnalysisReport &report, const UString &message)
{
    report.errorMessage = message;
    report.errorIdentity = UString();
    report.errorHasOffset = false;
    report.errorOffset = 0;
}

static bool includesDriver(uint32_t mask, size_t driver) { return (mask & ((uint32_t)1 << driver)) != 0; }
static bool excludesDriver(uint32_t mask, size_t driver) { return (mask & ((uint32_t)1 << driver)) == 0; }

typedef bool (*TerminalPredicate)(uint32_t mask, size_t driver);

// Lexicographically smallest full sequence from a state to a terminal state
// satisfying the predicate. Parent pointers keep history out of every edge.
struct WitnessSolver {
    const std::vector<std::vector<uint8_t> > &choices;
    std::vector<char>     memo;       // 0 unknown, 1 reaches target, 2 cannot
    std::vector<int>      nextDriver;
    std::vector<uint32_t> nextMask;

    WitnessSolver(const std::vector<std::vector<uint8_t> > &allChoices, size_t states)
        : choices(allChoices), memo(states, 0), nextDriver(states, -1), nextMask(states, 0) {}

    bool solve(uint32_t mask, TerminalPredicate predicate, size_t driver)
    {
        char cached = memo[mask];
        if (cached != 0)
            return cached == 1;

        if (choices[mask].empty()) {
            bool ok = predicate(mask, driver);
            memo[mask] = ok ? 1 : 2;
            return ok;
        }
        // Choices are stored in normalized driver order, i.e. ascending.
        for (size_t i = 0; i < choices[mask].size(); i++) {
            size_t k = choices[mask][i];
            uint32_t child = mask | ((uint32_t)1 << k);
            if (solve(child, predicate, driver)) {
                memo[mask] = 1;
                nextDriver[mask] = (int)k;
                nextMask[mask] = child;
                return true;
            }
        }
        memo[mask] = 2;
        return false;
    }
};

// Internal evaluation against a compact protocol bitmask. GUIDs absent from
// the analysis-wide index table can never be installed and read as false.
static bool evaluateBits(const DecodedDepex &decoded,
                         const std::map<EFI_GUID, size_t, GuidLess> &guidIndex,
                         const std::vector<uint64_t> &bits)
{
    std::vector<bool> stack;
    stack.reserve(decoded.operations.size());

    for (size_t i = 0; i < decoded.operations.size(); i++) {
        const Operation &op = decoded.operations[i];
        switch (op.kind) {
            case OpPush: {
                bool present = false;
                std::map<EFI_GUID, size_t, GuidLess>::const_iterator it = guidIndex.find(op.guid);
                if (it != guidIndex.end()) {
                    size_t index = it->second;
                    present = (bits[index / 64] & ((uint64_t)1 << (index % 64))) != 0;
                }
                stack.push_back(present);
                break;
            }
            case OpTrue:
                stack.push_back(true);
                break;
            case OpFalse:
                stack.push_back(false);
                break;
            case OpNot:
                stack.back() = !stack.back();
                break;
            case OpAnd: {
                bool rhs = stack.back();
                stack.pop_back();
                stack.back() = stack.back() && rhs;
                break;
            }
            case OpOr: {
                bool rhs = stack.back();
                stack.pop_back();
                stack.back() = stack.back() || rhs;
                break;
            }
        }
    }

    return stack.empty() ? false : stack.back();
}

static std::vector<uint64_t> makeBits(const std::map<EFI_GUID, size_t, GuidLess> &guidIndex,
                                      const GuidSet &protocols, size_t words)
{
    std::vector<uint64_t> bits(words, 0);
    for (GuidSet::const_iterator it = protocols.begin(); it != protocols.end(); ++it) {
        std::map<EFI_GUID, size_t, GuidLess>::const_iterator gi = guidIndex.find(*it);
        if (gi != guidIndex.end())
            bits[gi->second / 64] |= (uint64_t)1 << (gi->second % 64);
    }
    return bits;
}

// Reconstruct the human-readable installed set of a state for witness steps.
static GuidSet protocolsForWitness(uint32_t mask, const std::vector<PreparedDriver> &prepared,
                                   const GuidSet &initial)
{
    GuidSet protocols = initial;
    for (size_t k = 0; k < prepared.size(); k++) {
        if (mask & ((uint32_t)1 << k))
            protocols.insert(prepared[k].published.begin(), prepared[k].published.end());
    }
    return protocols;
}

bool analyze(const AnalysisInput &input, AnalysisReport &report)
{
    report = AnalysisReport();

    if (!input.volume.isValid() || input.volume.model()->type(input.volume) != Types::Volume) {
        failNoIdentity(report, UString("analysis requires a valid firmware volume node"));
        return false;
    }
    const TreeModel *model = input.volume.model();
    const size_t n = input.drivers.size();
    if (n > MAX_DRIVERS) {
        failNoIdentity(report, usprintf("analysis supports at most %u selected drivers, got %u",
                                        (unsigned)MAX_DRIVERS, (unsigned)n));
        return false;
    }

    // Validate selection: real file nodes, no duplicate selection, every node
    // belonging to the supplied volume.
    for (size_t i = 0; i < n; i++) {
        const UModelIndex &file = input.drivers[i].file;
        if (!file.isValid() || model->type(file) != Types::File) {
            failNoIdentity(report, UString("selected driver node is not a file of the supplied model"));
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (input.drivers[j].file == file) {
                NodeId id = nodeIdOf(input.volume, file);
                fail(report, UString("driver file is selected more than once"), id, false, 0);
                return false;
            }
        }
        if (!belongsToVolume(input.volume, file)) {
            NodeId id = nodeIdOf(UModelIndex(), file);
            fail(report, UString("selected driver file does not belong to the supplied firmware volume"), id, false, 0);
            return false;
        }
    }

    // Extract and strictly decode exactly one DXE_DEPEX per selected driver.
    struct ExtractedDriver {
        NodeId      identity;
        UByteArray  depexBody;
        NodeId      depexSection;
    };
    std::vector<ExtractedDriver> extracted;
    extracted.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const UModelIndex &file = input.drivers[i].file;
        NodeId fileId = nodeIdOf(input.volume, file);

        std::vector<std::pair<NodeId, UByteArray> > found;
        if (!collectDepex(input.volume, file, fileId, found, report))
            return false;
        if (found.empty()) {
            fail(report, UString("driver file has no DXE_DEPEX section"), fileId, false, 0);
            return false;
        }
        if (found.size() > 1) {
            fail(report, usprintf("driver file contains %u DXE_DEPEX sections, exactly one is required", (unsigned)found.size()),
                 fileId, false, 0);
            return false;
        }

        ExtractedDriver ed;
        ed.identity = fileId;
        ed.depexBody = found[0].second;
        ed.depexSection = found[0].first;
        extracted.push_back(ed);
    }

    // Normalize driver order by (path, base); results are independent of the
    // ordering of the caller supplied lists.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++)
        order[i] = i;
    for (size_t a = 1; a < n; a++) {
        size_t key = order[a];
        size_t b = a;
        while (b > 0 && NodeIdLess()(extracted[key].identity, extracted[order[b - 1]].identity)) {
            order[b] = order[b - 1];
            b--;
        }
        order[b] = key;
    }

    std::vector<PreparedDriver> prepared(n);
    for (size_t k = 0; k < n; k++) {
        const size_t src = order[k];
        PreparedDriver &pd = prepared[k];
        pd.identity = extracted[src].identity;
        const std::vector<EFI_GUID> &protocols = input.drivers[src].protocols;
        pd.publishedList = protocols;
        for (size_t g = 0; g < protocols.size(); g++)
            pd.published.insert(protocols[g]);

        DecodeError decodeError;
        if (!decode(extracted[src].depexBody, pd.depex, decodeError)) {
            fail(report, decodeError.message, pd.identity, true, decodeError.offset);
            return false;
        }
        pd.sor = pd.depex.sor;
    }

    // Resolve the optional apriori list against the normalized selection.
    std::vector<size_t> apriori;
    {
        std::vector<bool> seen(n, false);
        for (size_t a = 0; a < input.options.aprioriFiles.size(); a++) {
            const UModelIndex &apFile = input.options.aprioriFiles[a];
            size_t target = n;
            for (size_t k = 0; k < n; k++) {
                if (input.drivers[order[k]].file == apFile) {
                    target = k;
                    break;
                }
            }
            if (target == n) {
                NodeId id = apFile.isValid() ? nodeIdOf(input.volume, apFile) : NodeId(UString("/"), 0);
                fail(report, UString("apriori list references a file that is not part of the selected drivers"), id, false, 0);
                return false;
            }
            if (seen[target]) {
                fail(report, UString("apriori list references the same driver more than once"), prepared[target].identity, false, 0);
                return false;
            }
            seen[target] = true;
            apriori.push_back(target);
        }
    }

    // Apriori drivers execute first, once, with dependencies ignored.
    uint32_t baseMask = 0;
    GuidSet baseProtocols = input.initialProtocols;
    for (size_t a = 0; a < apriori.size(); a++) {
        size_t k = apriori[a];
        baseMask |= (uint32_t)1 << k;
        const GuidSet &pub = prepared[k].published;
        baseProtocols.insert(pub.begin(), pub.end());
    }

    // Index every distinct protocol GUID so a state's installed set fits in
    // a compact bitmask; protocol sets depend only on the executed set.
    std::map<EFI_GUID, size_t, GuidLess> guidIndex;
    GuidSet allProtocols = baseProtocols;
    for (size_t k = 0; k < n; k++)
        allProtocols.insert(prepared[k].published.begin(), prepared[k].published.end());
    size_t guidCount = 0;
    for (GuidSet::const_iterator it = allProtocols.begin(); it != allProtocols.end(); ++it)
        guidIndex[*it] = guidCount++;
    const size_t guidWords = guidCount == 0 ? 0 : (guidCount - 1) / 64 + 1;

    // Published set per driver and the initial state, all in bitmask form.
    std::vector<std::vector<uint64_t> > driverBits(n, std::vector<uint64_t>(guidWords, 0));
    for (size_t k = 0; k < n; k++)
        driverBits[k] = makeBits(guidIndex, prepared[k].published, guidWords);
    std::vector<uint64_t> baseBits = makeBits(guidIndex, baseProtocols, guidWords);

    // One expansion per executed-set state; never enumerate permutation trees.
    std::vector<uint32_t> stack;
    std::set<uint32_t> expanded;
    std::vector<std::vector<uint8_t> > choicesByMask((size_t)1 << n);
    std::vector<std::vector<uint64_t> > protocolBitsByMask((size_t)1 << n);
    std::vector<uint32_t> terminalMasks;
    size_t transitions = 0;

    stack.push_back(baseMask);
    protocolBitsByMask[baseMask] = baseBits;
    while (!stack.empty()) {
        uint32_t mask = stack.back();
        stack.pop_back();
        if (expanded.find(mask) != expanded.end())
            continue;
        expanded.insert(mask);

        const std::vector<uint64_t> &bits = protocolBitsByMask[mask];
        std::vector<uint8_t> choices;
        for (size_t k = 0; k < n; k++) {
            uint32_t bit = (uint32_t)1 << k;
            if (mask & bit)
                continue; // Every driver executes at most once
            if (prepared[k].sor &&
                input.options.sorRequests.find(prepared[k].identity) == input.options.sorRequests.end())
                continue; // SOR drivers additionally need a pending request
            if (!evaluateBits(prepared[k].depex, guidIndex, bits))
                continue;
            choices.push_back((uint8_t)k);
        }
        choicesByMask[mask] = choices;

        if (choices.empty()) {
            terminalMasks.push_back(mask);
            continue;
        }

        for (size_t c = 0; c < choices.size(); c++) {
            size_t k = choices[c];
            uint32_t next = mask | ((uint32_t)1 << k);
            transitions++;
            if (protocolBitsByMask[next].empty() && guidWords != 0) {
                std::vector<uint64_t> nextBits = bits;
                for (size_t w = 0; w < guidWords; w++)
                    nextBits[w] |= driverBits[k][w];
                protocolBitsByMask[next] = nextBits;
            }
            if (expanded.find(next) == expanded.end())
                stack.push_back(next);
        }
    }

    // may: some maximal schedule executes the driver.
    // must: every maximal schedule executes the driver.
    std::vector<bool> may(n, false), must(n, false);
    for (size_t k = 0; k < n; k++) {
        uint32_t bit = (uint32_t)1 << k;
        bool someWith = false, someWithout = false;
        for (size_t t = 0; t < terminalMasks.size(); t++) {
            if (terminalMasks[t] & bit)
                someWith = true;
            else
                someWithout = true;
        }
        may[k] = someWith;
        must[k] = !someWithout;
    }

    auto buildWitness = [&](const std::vector<size_t> &sequence) -> Witness {
        Witness witness;
        witness.present = true;

        GuidSet protocols = input.initialProtocols;
        uint32_t mask = baseMask;

        // Apriori prefix: dependencies are ignored, expression recorded anyway.
        for (size_t a = 0; a < apriori.size(); a++) {
            size_t k = apriori[a];
            WitnessStep step;
            step.driver = prepared[k].identity;
            step.apriori = true;
            step.satisfiedProtocols = protocols;
            step.expressionResult = evaluate(prepared[k].depex, protocols);
            witness.steps.push_back(step);
            const GuidSet &pub = prepared[k].published;
            protocols.insert(pub.begin(), pub.end());
        }
        // Normal phase: every recorded expression must be true at choice time.
        for (size_t i = 0; i < sequence.size(); i++) {
            size_t k = sequence[i];
            WitnessStep step;
            step.driver = prepared[k].identity;
            step.apriori = false;
            // Reconstruct the installed set solely from the executed set,
            // which is what determines it.
            step.satisfiedProtocols = protocolsForWitness(mask, prepared, input.initialProtocols);
            step.expressionResult = evaluate(prepared[k].depex, step.satisfiedProtocols);
            witness.steps.push_back(step);
            mask |= (uint32_t)1 << k;
            const GuidSet &pub = prepared[k].published;
            protocols.insert(pub.begin(), pub.end());
        }
        return witness;
    };

    auto sequenceFor = [&](TerminalPredicate predicate, size_t driver) -> Witness {
        WitnessSolver solver(choicesByMask, (size_t)1 << n);
        if (!solver.solve(baseMask, predicate, driver))
            return Witness(); // Absent witness, distinct from an empty schedule
        std::vector<size_t> sequence;
        uint32_t mask = baseMask;
        while (!choicesByMask[mask].empty()) {
            int k = solver.nextDriver[mask];
            if (k < 0)
                break;
            sequence.push_back((size_t)k);
            mask = solver.nextMask[mask];
        }
        return buildWitness(sequence);
    };

    report.drivers.reserve(n);
    for (size_t k = 0; k < n; k++) {
        DriverReport dr;
        dr.identity = prepared[k].identity;
        dr.depexSection = extracted[order[k]].depexSection;
        dr.sor = prepared[k].sor;
        dr.depex = prepared[k].depex; // Owned bytes and strict expression
        dr.publishedProtocols = prepared[k].publishedList;
        dr.may = may[k];
        dr.must = must[k];
        if (may[k])
            dr.mayWitness = sequenceFor(includesDriver, k);
        if (!must[k])
            dr.withoutWitness = sequenceFor(excludesDriver, k);
        report.drivers.push_back(dr);
    }

    report.expandedStates = expanded.size();
    report.transitions = transitions;
    return true;
}

} // namespace Depex
