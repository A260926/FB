/* regression.cpp

Copyright (c) 2026, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

// Regression tests for the Qt-free DepexAnalyzer. Every model and every
// DXE_DEPEX byte stream below is generated in C++; no firmware images, PE
// machine code inspection or driver loading are involved.

#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "support/check.h"

#include "common/treemodel.h"
#include "common/types.h"
#include "common/ffs.h"
#include "common/depex.h"

using namespace Depex;

// --------------------------------------------------------------------------
// Small construction helpers
// --------------------------------------------------------------------------

static EFI_GUID guidFromBytes(unsigned char first)
{
    unsigned char raw[16];
    for (int i = 0; i < 16; i++)
        raw[i] = (unsigned char)(first + (unsigned char)i);
    EFI_GUID guid;
    memcpy(&guid, raw, sizeof(EFI_GUID));
    return guid;
}

static UByteArray depexBytes(const unsigned char *opcodes, size_t size)
{
    return UByteArray(reinterpret_cast<const char *>(opcodes), (int32_t)size);
}

// PUSH <guid> style body builders, always terminated with END.
static UByteArray pushDepex(const EFI_GUID &guid, bool negate = false, bool sor = false)
{
    std::vector<unsigned char> bytes;
    if (sor)
        bytes.push_back(EFI_DEP_SOR);
    bytes.push_back(EFI_DEP_PUSH);
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(&guid);
    for (int i = 0; i < 16; i++)
        bytes.push_back(raw[i]);
    if (negate)
        bytes.push_back(EFI_DEP_NOT);
    bytes.push_back(EFI_DEP_END);
    return depexBytes(bytes.empty() ? 0 : &bytes[0], bytes.size());
}

static UByteArray constDepex(bool value, bool sor = false)
{
    unsigned char bytes[3];
    size_t n = 0;
    if (sor)
        bytes[n++] = EFI_DEP_SOR;
    bytes[n++] = value ? EFI_DEP_TRUE : EFI_DEP_FALSE;
    bytes[n++] = EFI_DEP_END;
    return depexBytes(bytes, n);
}

static UModelIndex addVolume(TreeModel &model)
{
    return model.addItem(0, Types::Volume, Subtypes::Ffs2Volume, "FV", "", "",
                         UByteArray(72, 0), UByteArray(), UByteArray(), Fixed);
}

static UModelIndex addDriverFile(TreeModel &model, const UModelIndex &volume, UINT32 offset)
{
    return model.addItem(offset, Types::File, EFI_FV_FILETYPE_DRIVER, "Driver", "", "",
                         UByteArray(24, 0), UByteArray(), UByteArray(), Fixed, volume);
}

static UModelIndex addSection(TreeModel &model, const UModelIndex &parent, UINT32 offset,
                               UINT8 subtype, const UByteArray &body)
{
    return model.addItem(offset, Types::Section, subtype, "Section", "", "",
                         UByteArray(4, 0), body, UByteArray(), Fixed, parent);
}

// A driver file carrying exactly one plain DXE_DEPEX section.
static UModelIndex simpleDriver(TreeModel &model, const UModelIndex &volume, UINT32 offset,
                                const UByteArray &depex)
{
    UModelIndex file = addDriverFile(model, volume, offset);
    addSection(model, file, 24, EFI_SECTION_DXE_DEPEX, depex);
    return file;
}

static DriverInput driverWith(const UModelIndex &file, const std::vector<EFI_GUID> &protocols)
{
    DriverInput input;
    input.file = file;
    input.protocols = protocols;
    return input;
}

static int indexOf(const AnalysisReport &report, const NodeId &id)
{
    for (size_t i = 0; i < report.drivers.size(); i++) {
        if (report.drivers[i].identity == id)
            return (int)i;
    }
    return -1;
}

// std::set::operator== would require EFI_GUID to provide operator==; the
// identity rule is the raw 16 on-disk bytes, so compare explicitly.
static bool guidSetsEqual(const GuidSet &lhs, const GuidSet &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    GuidSet::const_iterator a = lhs.begin();
    GuidSet::const_iterator b = rhs.begin();
    for (; a != lhs.end(); ++a, ++b) {
        if (memcmp(&(*a), &(*b), sizeof(EFI_GUID)) != 0)
            return false;
    }
    return true;
}

static bool nodeIdSetsEqual(const NodeIdSet &lhs, const NodeIdSet &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    NodeIdSet::const_iterator a = lhs.begin();
    NodeIdSet::const_iterator b = rhs.begin();
    for (; a != lhs.end(); ++a, ++b) {
        if (!(*a == *b))
            return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// Independent re-implementation of the dispatch semantics, used only by the
// tests to cross-check the analyzer instead of trusting its own bookkeeping.
// --------------------------------------------------------------------------

struct IndependentSpec {
    NodeId        identity;
    DecodedDepex  depex;
    GuidSet       published;
    bool          sor;
    bool          apriori;
};

struct IndependentResult {
    std::set<uint32_t>  states;
    std::vector<uint32_t> terminals;
    size_t              transitions;
    std::vector<bool>   may;
    std::vector<bool>   must;
};

static GuidSet protocolsForMask(uint32_t mask, const std::vector<IndependentSpec> &specs,
                                const GuidSet &initial)
{
    GuidSet protocols = initial;
    for (size_t k = 0; k < specs.size(); k++) {
        if (mask & ((uint32_t)1 << k))
            protocols.insert(specs[k].published.begin(), specs[k].published.end());
    }
    return protocols;
}

static IndependentResult independentRun(const std::vector<IndependentSpec> &specs,
                                        const GuidSet &initial,
                                        const NodeIdSet &sorRequests)
{
    const size_t n = specs.size();
    IndependentResult result;
    result.transitions = 0;
    result.may.assign(n, false);
    result.must.assign(n, false);

    uint32_t baseMask = 0;
    for (size_t k = 0; k < n; k++) {
        if (specs[k].apriori)
            baseMask |= (uint32_t)1 << k;
    }

    std::vector<uint32_t> stack;
    stack.push_back(baseMask);
    while (!stack.empty()) {
        uint32_t mask = stack.back();
        stack.pop_back();
        if (result.states.count(mask))
            continue;
        result.states.insert(mask);

        GuidSet protocols = protocolsForMask(mask, specs, initial);
        std::vector<size_t> choices;
        for (size_t k = 0; k < n; k++) {
            if (mask & ((uint32_t)1 << k))
                continue;
            if (specs[k].sor && sorRequests.count(specs[k].identity) == 0)
                continue;
            if (!evaluate(specs[k].depex, protocols))
                continue;
            choices.push_back(k);
        }
        if (choices.empty()) {
            result.terminals.push_back(mask);
            continue;
        }
        for (size_t c = 0; c < choices.size(); c++) {
            uint32_t next = mask | ((uint32_t)1 << choices[c]);
            result.transitions++;
            if (!result.states.count(next))
                stack.push_back(next);
        }
    }

    for (size_t k = 0; k < n; k++) {
        bool with = false, without = false;
        for (size_t t = 0; t < result.terminals.size(); t++) {
            if (result.terminals[t] & ((uint32_t)1 << k))
                with = true;
            else
                without = true;
        }
        result.may[k] = with;
        result.must[k] = !without;
    }
    return result;
}

static std::vector<IndependentSpec> specsFromReport(const AnalysisReport &report,
                                                    const NodeIdSet &aprioriIds)
{
    std::vector<IndependentSpec> specs;
    for (size_t i = 0; i < report.drivers.size(); i++) {
        const DriverReport &dr = report.drivers[i];
        IndependentSpec spec;
        spec.identity = dr.identity;
        spec.depex = dr.depex;
        spec.sor = dr.sor;
        spec.apriori = aprioriIds.count(dr.identity) != 0;
        for (size_t g = 0; g < dr.publishedProtocols.size(); g++)
            spec.published.insert(dr.publishedProtocols[g]);
        specs.push_back(spec);
    }
    return specs;
}

// Replay a witness from the report-owned data alone and prove executability,
// termination and maximality. targetMode: -1 no target check, 0 must exclude
// target, 1 must include target.
static void replayWitness(const AnalysisReport &report, const Witness &witness,
                          const GuidSet &initial, const NodeIdSet &sorRequests,
                          const NodeIdSet &aprioriIds,
                          const NodeId *target, int targetMode)
{
    UNIT_CHECK(witness.present);
    std::set<NodeId, NodeIdLess> executed;
    GuidSet protocols = initial;
    std::set<NodeId, NodeIdLess> seenApriori;

    for (size_t step = 0; step < witness.steps.size(); step++) {
        const WitnessStep &s = witness.steps[step];
        int idx = indexOf(report, s.driver);
        UNIT_CHECK(idx >= 0);
        const DriverReport &dr = report.drivers[(size_t)idx];

        // Identity must be unique inside one schedule.
        UNIT_CHECK(executed.count(s.driver) == 0);
        executed.insert(s.driver);

        // The evidence block must describe the protocols at exactly this point.
        UNIT_CHECK(guidSetsEqual(s.satisfiedProtocols, protocols));
        UNIT_CHECK(s.expressionResult == evaluate(dr.depex, protocols));

        if (s.apriori) {
            UNIT_CHECK(aprioriIds.count(s.driver) != 0);
            seenApriori.insert(s.driver);
        } else {
            // A normal step is only legal while its expression is true ...
            UNIT_CHECK(s.expressionResult == true);
            // ... and a SOR driver additionally needs an outstanding request.
            if (dr.sor)
                UNIT_CHECK(sorRequests.count(s.driver) != 0);
        }

        for (size_t g = 0; g < dr.publishedProtocols.size(); g++)
            protocols.insert(dr.publishedProtocols[g]);
    }
    UNIT_CHECK(nodeIdSetsEqual(seenApriori, aprioriIds));

    if (target) {
        bool contains = executed.count(*target) != 0;
        if (targetMode == 0)
            UNIT_CHECK(!contains);
        else if (targetMode == 1)
            UNIT_CHECK(contains);
    }

    // Termination: no remaining driver may be selected, so the schedule is
    // maximal rather than stopped early.
    for (size_t i = 0; i < report.drivers.size(); i++) {
        const DriverReport &dr = report.drivers[i];
        if (executed.count(dr.identity))
            continue;
        bool blockedBySor = dr.sor && sorRequests.count(dr.identity) == 0;
        UNIT_CHECK(blockedBySor || evaluate(dr.depex, protocols) == false);
    }
}

static void checkMayMustAndWitnesses(const AnalysisReport &report,
                                     const IndependentResult &expected,
                                     const GuidSet &initial,
                                     const NodeIdSet &sorRequests,
                                     const NodeIdSet &aprioriIds)
{
    UNIT_CHECK(report.ok());
    UNIT_CHECK(report.drivers.size() == expected.may.size());
    UNIT_CHECK(report.expandedStates == expected.states.size());
    UNIT_CHECK(report.transitions == expected.transitions);

    // Report order must be normalized by identity.
    for (size_t i = 1; i < report.drivers.size(); i++)
        UNIT_CHECK(NodeIdLess()(report.drivers[i - 1].identity, report.drivers[i].identity));

    for (size_t i = 0; i < report.drivers.size(); i++) {
        const DriverReport &dr = report.drivers[i];
        UNIT_CHECK(dr.may == expected.may[i]);
        UNIT_CHECK(dr.must == expected.must[i]);

        if (dr.may) {
            UNIT_CHECK(dr.mayWitness.present);
            replayWitness(report, dr.mayWitness, initial, sorRequests, aprioriIds,
                          &dr.identity, 1);
        } else {
            UNIT_CHECK(!dr.mayWitness.present);
            UNIT_CHECK(dr.mayWitness.steps.empty());
        }

        if (!dr.must) {
            UNIT_CHECK(dr.withoutWitness.present);
            replayWitness(report, dr.withoutWitness, initial, sorRequests, aprioriIds,
                          &dr.identity, 0);
        } else {
            UNIT_CHECK(!dr.withoutWitness.present);
            UNIT_CHECK(dr.withoutWitness.steps.empty());
        }
    }
}

// --------------------------------------------------------------------------
// normal group
// --------------------------------------------------------------------------

// The example from the task statement:
// A depends on NOT P and publishes Q, B depends on TRUE and publishes P,
// C depends on Q. B is must; A and C are may only.
static void test_not_window_dispatch(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0x10);
    const EFI_GUID guidQ = guidFromBytes(0x30);

    UModelIndex fileA = simpleDriver(model, volume, 72,  pushDepex(guidP, true));
    UModelIndex fileB = simpleDriver(model, volume, 140, constDepex(true));
    UModelIndex fileC = simpleDriver(model, volume, 208, pushDepex(guidQ));

    AnalysisInput input;
    input.volume = volume;
    input.drivers.push_back(driverWith(fileA, std::vector<EFI_GUID>(1, guidQ)));
    input.drivers.push_back(driverWith(fileB, std::vector<EFI_GUID>(1, guidP)));
    input.drivers.push_back(driverWith(fileC, std::vector<EFI_GUID>()));

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));

    NodeIdSet empty;
    IndependentResult expected = independentRun(specsFromReport(report, empty), GuidSet(), empty);
    checkMayMustAndWitnesses(report, expected, GuidSet(), empty, empty);

    int a = indexOf(report, nodeIdOf(volume, fileA));
    int b = indexOf(report, nodeIdOf(volume, fileB));
    int c = indexOf(report, nodeIdOf(volume, fileC));
    UNIT_CHECK(a >= 0 && b >= 0 && c >= 0);
    // For a plain driver the DEPEX identity is the single leaf section.
    UNIT_CHECK(report.drivers[a].depexSection.path
               == nodeIdOf(volume, model.index(0, 0, fileA)).path);
    UNIT_CHECK(report.drivers[a].may && !report.drivers[a].must);
    UNIT_CHECK(report.drivers[b].may &&  report.drivers[b].must);
    UNIT_CHECK(report.drivers[c].may && !report.drivers[c].must);

    // Exhaustive exploration over executed sets: {}, {B}, {A}, {A,B},
    // {A,C} and {A,B,C} - six merged states with six transitions, never the
    // permutation tree (two distinct paths converge on {A,B,C}).
    UNIT_CHECK(report.expandedStates == 6);
    UNIT_CHECK(report.transitions == 6);

    // The schedule without A is the one where B went first and stranded C too.
    const Witness &withoutA = report.drivers[a].withoutWitness;
    UNIT_CHECK(withoutA.present);
    UNIT_CHECK(withoutA.steps.size() == 1);
    UNIT_CHECK(withoutA.steps[0].driver == report.drivers[b].identity);
}

// Repeated protocol suppliers and an initially installed protocol: a driver
// depending on NOT P can never be selected once P is present from the start.
static void test_duplicate_suppliers_and_initial(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0x40);

    UModelIndex fileB1 = simpleDriver(model, volume, 72,  constDepex(true));
    UModelIndex fileB2 = simpleDriver(model, volume, 140, constDepex(true));
    UModelIndex fileA  = simpleDriver(model, volume, 208, pushDepex(guidP));
    UModelIndex fileX  = simpleDriver(model, volume, 276, pushDepex(guidP, true));

    AnalysisInput input;
    input.volume = volume;
    input.initialProtocols.insert(guidP);
    input.drivers.push_back(driverWith(fileB1, std::vector<EFI_GUID>(1, guidP)));
    input.drivers.push_back(driverWith(fileB2, std::vector<EFI_GUID>(1, guidP)));
    input.drivers.push_back(driverWith(fileA,  std::vector<EFI_GUID>()));
    input.drivers.push_back(driverWith(fileX,  std::vector<EFI_GUID>()));

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));

    NodeIdSet empty;
    IndependentResult expected = independentRun(specsFromReport(report, empty),
                                                input.initialProtocols, empty);
    checkMayMustAndWitnesses(report, expected, input.initialProtocols, empty, empty);

    int b1 = indexOf(report, nodeIdOf(volume, fileB1));
    int b2 = indexOf(report, nodeIdOf(volume, fileB2));
    int a  = indexOf(report, nodeIdOf(volume, fileA));
    int x  = indexOf(report, nodeIdOf(volume, fileX));
    UNIT_CHECK(report.drivers[b1].must && report.drivers[b2].must && report.drivers[a].must);
    UNIT_CHECK(!report.drivers[x].may && !report.drivers[x].must);
    // The absent may-witness must be distinguishable from a present empty one.
    UNIT_CHECK(!report.drivers[x].mayWitness.present);
    UNIT_CHECK(report.drivers[x].withoutWitness.present);
    UNIT_CHECK(report.drivers[x].withoutWitness.steps.size() == 3);
}

// A cyclic dependency deadlocks immediately: the only terminal is the empty
// schedule, which is present but carries no steps.
static void test_cyclic_dependency(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0x60);
    const EFI_GUID guidQ = guidFromBytes(0x80);

    UModelIndex fileA = simpleDriver(model, volume, 72,  pushDepex(guidP));
    UModelIndex fileB = simpleDriver(model, volume, 140, pushDepex(guidQ));

    AnalysisInput input;
    input.volume = volume;
    std::vector<EFI_GUID> publishesP(1, guidP);
    std::vector<EFI_GUID> publishesQ(1, guidQ);
    input.drivers.push_back(driverWith(fileA, publishesQ)); // A needs P, gives Q
    input.drivers.push_back(driverWith(fileB, publishesP)); // B needs Q, gives P

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));

    NodeIdSet empty;
    IndependentResult expected = independentRun(specsFromReport(report, empty), GuidSet(), empty);
    checkMayMustAndWitnesses(report, expected, GuidSet(), empty, empty);

    UNIT_CHECK(report.expandedStates == 1);
    UNIT_CHECK(report.transitions == 0);
    for (size_t i = 0; i < report.drivers.size(); i++) {
        UNIT_CHECK(!report.drivers[i].may);
        UNIT_CHECK(!report.drivers[i].must);
        UNIT_CHECK(!report.drivers[i].mayWitness.present);
        // Present empty schedule: nobody can ever run.
        UNIT_CHECK(report.drivers[i].withoutWitness.present);
        UNIT_CHECK(report.drivers[i].withoutWitness.steps.empty());
    }
}

// A constant FALSE dependency is never selectable, while two driver files
// carrying identical file-header GUID bytes must remain distinct identities.
static void test_constant_false_and_same_guid_copies(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);

    // Two files whose 16-byte file GUID header area is byte-for-byte identical,
    // placed at different offsets inside the same volume.
    unsigned char headerBytes[24];
    memset(headerBytes, 0, sizeof(headerBytes));
    const EFI_GUID sharedFileGuid = guidFromBytes(0xE0);
    memcpy(headerBytes, &sharedFileGuid, sizeof(EFI_GUID));
    UByteArray sameHeader(reinterpret_cast<const char *>(headerBytes), 24);

    UModelIndex file1 = model.addItem(72, Types::File, EFI_FV_FILETYPE_DRIVER, "Driver", "", "",
                                      sameHeader, UByteArray(), UByteArray(), Fixed, volume);
    addSection(model, file1, 24, EFI_SECTION_DXE_DEPEX, constDepex(false));
    UModelIndex file2 = model.addItem(200, Types::File, EFI_FV_FILETYPE_DRIVER, "Driver", "", "",
                                      sameHeader, UByteArray(), UByteArray(), Fixed, volume);
    addSection(model, file2, 24, EFI_SECTION_DXE_DEPEX, constDepex(true));

    AnalysisInput input;
    input.volume = volume;
    input.drivers.push_back(driverWith(file1, std::vector<EFI_GUID>()));
    input.drivers.push_back(driverWith(file2, std::vector<EFI_GUID>()));

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));

    NodeIdSet empty;
    IndependentResult expected = independentRun(specsFromReport(report, empty), GuidSet(), empty);
    checkMayMustAndWitnesses(report, expected, GuidSet(), empty, empty);

    NodeId id1 = nodeIdOf(volume, file1);
    NodeId id2 = nodeIdOf(volume, file2);
    UNIT_CHECK(!(id1 == id2));
    int i1 = indexOf(report, id1);
    int i2 = indexOf(report, id2);
    UNIT_CHECK(i1 >= 0 && i2 >= 0 && i1 != i2);

    // FALSE driver: never executed, yet a maximal schedule excluding it exists.
    UNIT_CHECK(!report.drivers[i1].may && !report.drivers[i1].must);
    UNIT_CHECK(!report.drivers[i1].mayWitness.present);
    UNIT_CHECK(report.drivers[i1].withoutWitness.present);
    UNIT_CHECK(report.drivers[i1].withoutWitness.steps.size() == 1);
    UNIT_CHECK(report.drivers[i1].withoutWitness.steps[0].driver == id2);
    UNIT_CHECK(evaluate(report.drivers[i1].depex, GuidSet()) == false);

    // TRUE driver runs in every maximal schedule.
    UNIT_CHECK(report.drivers[i2].may && report.drivers[i2].must);

    UNIT_CHECK(report.expandedStates == 2);
    UNIT_CHECK(report.transitions == 1);
}

// --------------------------------------------------------------------------
// boundary group
// --------------------------------------------------------------------------

static void test_strict_depex_byte_validation(void)
{
    // The shared renderer must keep the historical one-opcode-per-line order
    // that FfsParser used to emit for valid streams.
    {
        DecodedDepex decoded;
        DecodeError error;
        unsigned char simple[] = {EFI_DEP_TRUE, EFI_DEP_END};
        UNIT_CHECK(decode(depexBytes(simple, 2), decoded, error));
        UNIT_CHECK(renderText(decoded) == UString("\nTRUE\nEND"));

        const EFI_GUID guid = guidFromBytes(0x50);
        UByteArray sorBody = pushDepex(guid, true, true); // SOR PUSH NOT END
        UNIT_CHECK(decode(sorBody, decoded, error));
        UNIT_CHECK(decoded.sor);
        UString text = renderText(decoded);
        UNIT_CHECK(text.find("\nSOR") == 0);
        UNIT_CHECK(text.find("\nPUSH ") >= 0);
        UNIT_CHECK(text.find("\nNOT") > text.find("\nPUSH "));
        UNIT_CHECK(text.find("\nEND") > text.find("\nNOT"));
        UNIT_CHECK(decoded.body.size() == sorBody.size());

        // A well-formed AND/OR expression evaluates against installed sets,
        // still consuming the complete stream rather than short-circuiting.
        const EFI_GUID guidR = guidFromBytes(0x55);
        unsigned char compound[] = {
            EFI_DEP_PUSH, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            EFI_DEP_TRUE, EFI_DEP_AND,
            EFI_DEP_FALSE, EFI_DEP_OR,
            EFI_DEP_END
        };
        memcpy(&compound[1], &guidR, sizeof(EFI_GUID));
        DecodedDepex compoundDecoded;
        UNIT_CHECK(decode(depexBytes(compound, sizeof(compound)), compoundDecoded, error));
        GuidSet installed;
        UNIT_CHECK(evaluate(compoundDecoded, installed) == false); // (R AND TRUE) OR FALSE
        installed.insert(guidR);
        UNIT_CHECK(evaluate(compoundDecoded, installed) == true);
    }

    TreeModel model;
    UModelIndex volume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0xA0);

    struct BadCase {
        const char *name;
        UByteArray  body;
        UINT32      offset;
    };
    std::vector<BadCase> cases;

    // PUSH with only 15 GUID bytes following the opcode.
    unsigned char truncated[16];
    truncated[0] = EFI_DEP_PUSH;
    for (int i = 1; i < 16; i++)
        truncated[i] = (unsigned char)i;
    cases.push_back(BadCase{"truncated push", depexBytes(truncated, 16), 0});

    // Stack underflow hidden behind a TRUE that would already decide the value.
    unsigned char underflow[] = {EFI_DEP_TRUE, EFI_DEP_AND, EFI_DEP_END};
    cases.push_back(BadCase{"and underflow", depexBytes(underflow, 3), 1});

    // The same structural requirement applies to OR and NOT.
    unsigned char orUnderflow[] = {EFI_DEP_FALSE, EFI_DEP_OR, EFI_DEP_END};
    cases.push_back(BadCase{"or underflow", depexBytes(orUnderflow, 3), 1});
    unsigned char notUnderflow[] = {EFI_DEP_NOT, EFI_DEP_END};
    cases.push_back(BadCase{"not underflow", depexBytes(notUnderflow, 2), 0});

    // END with two values on the stack.
    unsigned char twoValues[] = {EFI_DEP_TRUE, EFI_DEP_TRUE, EFI_DEP_END};
    cases.push_back(BadCase{"two values at end", depexBytes(twoValues, 3), 2});

    // Garbage after END.
    unsigned char garbage[] = {EFI_DEP_TRUE, EFI_DEP_END, EFI_DEP_TRUE};
    cases.push_back(BadCase{"garbage after end", depexBytes(garbage, 3), 2});

    // Unknown opcode.
    unsigned char unknown[] = {0x0A, EFI_DEP_END};
    cases.push_back(BadCase{"unknown opcode", depexBytes(unknown, 2), 0});

    // BEFORE ordering opcode is unsupported by the analyzer.
    unsigned char before[18];
    before[0] = EFI_DEP_BEFORE;
    const unsigned char *rawGuid = reinterpret_cast<const unsigned char *>(&guidP);
    for (int i = 0; i < 16; i++)
        before[i + 1] = rawGuid[i];
    before[17] = EFI_DEP_END;
    cases.push_back(BadCase{"before unsupported", depexBytes(before, 18), 0});

    // AFTER ordering opcode is unsupported in exactly the same way.
    unsigned char after[18];
    after[0] = EFI_DEP_AFTER;
    for (int i = 0; i < 16; i++)
        after[i + 1] = rawGuid[i];
    after[17] = EFI_DEP_END;
    cases.push_back(BadCase{"after unsupported", depexBytes(after, 18), 0});

    // Misplaced SOR.
    unsigned char sorBad[] = {EFI_DEP_TRUE, EFI_DEP_SOR, EFI_DEP_END};
    cases.push_back(BadCase{"misplaced sor", depexBytes(sorBad, 3), 1});

    for (size_t i = 0; i < cases.size(); i++) {
        UModelIndex file = simpleDriver(model, volume, 72 + (UINT32)i * 68, cases[i].body);
        AnalysisInput input;
        input.volume = volume;
        input.drivers.push_back(driverWith(file, std::vector<EFI_GUID>()));
        AnalysisReport report;
        UNIT_CHECK(!analyze(input, report));
        UNIT_CHECK(!report.ok());
        UNIT_CHECK(report.errorHasOffset);
        UNIT_CHECK(report.errorOffset == cases[i].offset);
        UNIT_CHECK(!report.errorIdentity.isEmpty());
        UNIT_CHECK(report.drivers.empty());
    }

    // An empty body is rejected as well.
    UModelIndex emptyFile = addDriverFile(model, volume, 72 + (UINT32)cases.size() * 68);
    addSection(model, emptyFile, 24, EFI_SECTION_DXE_DEPEX, UByteArray());
    AnalysisInput emptyInput;
    emptyInput.volume = volume;
    emptyInput.drivers.push_back(driverWith(emptyFile, std::vector<EFI_GUID>()));
    AnalysisReport emptyReport;
    UNIT_CHECK(!analyze(emptyInput, emptyReport));
    UNIT_CHECK(emptyReport.errorHasOffset && emptyReport.errorOffset == 0);
}

static void test_selection_and_scope_rejection(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);
    UModelIndex otherVolume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0xC0);
    const UByteArray good = constDepex(true);

    // Missing DEPEX.
    UModelIndex noDepex = addDriverFile(model, volume, 72);
    addSection(model, noDepex, 24, EFI_SECTION_RAW, UByteArray(2, 0));

    AnalysisInput input;
    input.volume = volume;
    input.drivers.push_back(driverWith(noDepex, std::vector<EFI_GUID>()));
    AnalysisReport report;
    UNIT_CHECK(!analyze(input, report));
    UNIT_CHECK(!report.errorHasOffset);

    // Duplicate DEPEX sections in one file.
    UModelIndex twoDepex = addDriverFile(model, volume, 140);
    addSection(model, twoDepex, 24, EFI_SECTION_DXE_DEPEX, good);
    addSection(model, twoDepex, 32, EFI_SECTION_DXE_DEPEX, good);
    AnalysisInput input2;
    input2.volume = volume;
    input2.drivers.push_back(driverWith(twoDepex, std::vector<EFI_GUID>()));
    AnalysisReport report2;
    UNIT_CHECK(!analyze(input2, report2));

    // Duplicate selection of the same file.
    UModelIndex dupFile = simpleDriver(model, volume, 208, good);
    AnalysisInput input3;
    input3.volume = volume;
    input3.drivers.push_back(driverWith(dupFile, std::vector<EFI_GUID>()));
    input3.drivers.push_back(driverWith(dupFile, std::vector<EFI_GUID>()));
    AnalysisReport report3;
    UNIT_CHECK(!analyze(input3, report3));

    // File belonging to another volume.
    UModelIndex foreign = simpleDriver(model, otherVolume, 72, good);
    AnalysisInput input4;
    input4.volume = volume;
    input4.drivers.push_back(driverWith(foreign, std::vector<EFI_GUID>()));
    AnalysisReport report4;
    UNIT_CHECK(!analyze(input4, report4));

    // Unexpanded encapsulation: compression section without parser children.
    UModelIndex wrapped = addDriverFile(model, volume, 276);
    addSection(model, wrapped, 24, EFI_SECTION_COMPRESSION, UByteArray(8, 0));
    AnalysisInput input5;
    input5.volume = volume;
    input5.drivers.push_back(driverWith(wrapped, std::vector<EFI_GUID>()));
    AnalysisReport report5;
    UNIT_CHECK(!analyze(input5, report5));

    // PEI dependency sections are not treated as TRUE.
    UModelIndex pei = addDriverFile(model, volume, 344);
    addSection(model, pei, 24, EFI_SECTION_PEI_DEPEX, good);
    AnalysisInput input6;
    input6.volume = volume;
    input6.drivers.push_back(driverWith(pei, std::vector<EFI_GUID>()));
    AnalysisReport report6;
    UNIT_CHECK(!analyze(input6, report6));

    // MM dependency sections are rejected just like PEI ones.
    UModelIndex mm = addDriverFile(model, volume, 378);
    addSection(model, mm, 24, EFI_SECTION_MM_DEPEX, good);
    AnalysisInput input6b;
    input6b.volume = volume;
    input6b.drivers.push_back(driverWith(mm, std::vector<EFI_GUID>()));
    AnalysisReport report6b;
    UNIT_CHECK(!analyze(input6b, report6b));
    UNIT_CHECK(!report6b.errorHasOffset);

    // A GUID defined encapsulation the parser never expanded is unsupported.
    UModelIndex unguided = addDriverFile(model, volume, 446);
    addSection(model, unguided, 24, EFI_SECTION_GUID_DEFINED, UByteArray(20, 0));
    AnalysisInput input6c;
    input6c.volume = volume;
    input6c.drivers.push_back(driverWith(unguided, std::vector<EFI_GUID>()));
    AnalysisReport report6c;
    UNIT_CHECK(!analyze(input6c, report6c));
    UNIT_CHECK(!report6c.errorHasOffset);

    // Apriori may only reference selected files.
    UModelIndex aprioriFile = simpleDriver(model, volume, 412, good);
    UModelIndex outsider = simpleDriver(model, otherVolume, 140, good);
    AnalysisInput input7;
    input7.volume = volume;
    input7.drivers.push_back(driverWith(aprioriFile, std::vector<EFI_GUID>(1, guidP)));
    input7.options.aprioriFiles.push_back(outsider);
    AnalysisReport report7;
    UNIT_CHECK(!analyze(input7, report7));

    // Duplicate apriori entries.
    AnalysisInput input8;
    input8.volume = volume;
    input8.drivers.push_back(driverWith(aprioriFile, std::vector<EFI_GUID>(1, guidP)));
    input8.options.aprioriFiles.push_back(aprioriFile);
    input8.options.aprioriFiles.push_back(aprioriFile);
    AnalysisReport report8;
    UNIT_CHECK(!analyze(input8, report8));

    // More than 18 selected drivers is rejected outright.
    AnalysisInput input9;
    input9.volume = volume;
    for (size_t i = 0; i < 19; i++) {
        UModelIndex file = simpleDriver(model, volume, 500 + (UINT32)i * 68, good);
        input9.drivers.push_back(driverWith(file, std::vector<EFI_GUID>()));
    }
    AnalysisReport report9;
    UNIT_CHECK(!analyze(input9, report9));
}

// 16 independent TRUE drivers must explore 2^16 executed sets, not 16!
// permutations. An empty selection yields a single terminal state.
static void test_state_space_bounds(void)
{
    AnalysisReport emptyReport;
    AnalysisInput emptyInput;
    TreeModel emptyModel;
    emptyInput.volume = addVolume(emptyModel);
    UNIT_CHECK(analyze(emptyInput, emptyReport));
    UNIT_CHECK(emptyReport.ok());
    UNIT_CHECK(emptyReport.drivers.empty());
    UNIT_CHECK(emptyReport.expandedStates == 1);
    UNIT_CHECK(emptyReport.transitions == 0);

    TreeModel model;
    UModelIndex volume = addVolume(model);
    AnalysisInput input;
    input.volume = volume;
    const size_t count = 16;
    for (size_t i = 0; i < count; i++) {
        UModelIndex file = simpleDriver(model, volume, 72 + (UINT32)i * 68, constDepex(true));
        input.drivers.push_back(driverWith(file, std::vector<EFI_GUID>()));
    }

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));
    UNIT_CHECK(report.drivers.size() == count);
    UNIT_CHECK(report.expandedStates == (size_t)1 << 16);
    UNIT_CHECK(report.transitions == count * ((size_t)1 << 15));
    for (size_t i = 0; i < count; i++) {
        UNIT_CHECK(report.drivers[i].may && report.drivers[i].must);
        UNIT_CHECK(report.drivers[i].mayWitness.present);
        UNIT_CHECK(report.drivers[i].mayWitness.steps.size() == count);
        UNIT_CHECK(!report.drivers[i].withoutWitness.present);
    }

    // With many equally valid orderings, the stored witness must be the
    // lexicographically smallest complete sequence, i.e. ascending identity.
    const Witness &lexMin = report.drivers[0].mayWitness;
    for (size_t i = 0; i < count; i++)
        UNIT_CHECK(lexMin.steps[i].driver == report.drivers[i].identity);
}

// --------------------------------------------------------------------------
// integration group
// --------------------------------------------------------------------------

// Nested expanded encapsulation, two file copies that must keep distinct
// identities, and invariance against permuted input lists.
static void test_nested_encapsulation_and_identities(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);
    const EFI_GUID guidP = guidFromBytes(0x10);
    const EFI_GUID guidQ = guidFromBytes(0x30);

    const UByteArray depexP = pushDepex(guidP);
    const UByteArray depexQ = pushDepex(guidQ);
    const UByteArray depexTrue = constDepex(true);

    // File at offset 72: file -> compression -> GUID defined -> DXE_DEPEX.
    UModelIndex file1 = addDriverFile(model, volume, 72);
    UModelIndex comp1 = addSection(model, file1, 24, EFI_SECTION_COMPRESSION, UByteArray(8, 0));
    UModelIndex guid1 = addSection(model, comp1, 8, EFI_SECTION_GUID_DEFINED, UByteArray(20, 0));
    addSection(model, guid1, 20, EFI_SECTION_DXE_DEPEX, depexP);

    // A second copy at a different offset with the same synthetic file name.
    UModelIndex file2 = addDriverFile(model, volume, 300);
    UModelIndex comp2 = addSection(model, file2, 24, EFI_SECTION_COMPRESSION, UByteArray(8, 0));
    UModelIndex guid2 = addSection(model, comp2, 8, EFI_SECTION_GUID_DEFINED, UByteArray(20, 0));
    addSection(model, guid2, 20, EFI_SECTION_DXE_DEPEX, depexQ);

    UModelIndex file3 = simpleDriver(model, volume, 540, depexTrue);

    AnalysisInput input;
    input.volume = volume;
    input.drivers.push_back(driverWith(file3, std::vector<EFI_GUID>()));
    input.drivers.push_back(driverWith(file1, std::vector<EFI_GUID>()));
    input.drivers.push_back(driverWith(file2, std::vector<EFI_GUID>()));

    AnalysisReport report;
    UNIT_CHECK(analyze(input, report));
    NodeIdSet empty;
    IndependentResult expected = independentRun(specsFromReport(report, empty), GuidSet(), empty);
    checkMayMustAndWitnesses(report, expected, GuidSet(), empty, empty);

    NodeId id1 = nodeIdOf(volume, file1);
    NodeId id2 = nodeIdOf(volume, file2);
    UNIT_CHECK(!(id1 == id2));
    UNIT_CHECK(indexOf(report, id1) >= 0 && indexOf(report, id2) >= 0);

    // Driver identity is the file node; the nested section path is still
    // encoded relative to the same volume.
    UNIT_CHECK(report.drivers[indexOf(report, id1)].identity.path.find("File@72") >= 0);
    UNIT_CHECK(report.drivers[indexOf(report, id1)].identity.base == 72);

    // The DEPEX section identity records the full traversal through both
    // expanded encapsulation layers.
    const NodeId &sectionId = report.drivers[indexOf(report, id1)].depexSection;
    UNIT_CHECK(sectionId.path.find("Section@24") >= 0); // compression
    UNIT_CHECK(sectionId.path.find("Section@8") >= 0);  // GUID defined
    UNIT_CHECK(sectionId.path.find("Section@20") >= 0); // DXE_DEPEX leaf

    // Rerun with a permuted driver order: conclusions and witnesses match.
    AnalysisInput shuffled = input;
    std::reverse(shuffled.drivers.begin(), shuffled.drivers.end());
    AnalysisReport report2;
    UNIT_CHECK(analyze(shuffled, report2));
    UNIT_CHECK(report2.drivers.size() == report.drivers.size());
    for (size_t i = 0; i < report.drivers.size(); i++) {
        const NodeId &id = report.drivers[i].identity;
        int j = indexOf(report2, id);
        UNIT_CHECK(j >= 0);
        UNIT_CHECK(report2.drivers[j].may == report.drivers[i].may);
        UNIT_CHECK(report2.drivers[j].must == report.drivers[i].must);
        UNIT_CHECK(report2.drivers[j].mayWitness.present
                   == report.drivers[i].mayWitness.present);
        UNIT_CHECK(report2.drivers[j].mayWitness.steps.size()
                   == report.drivers[i].mayWitness.steps.size());
        for (size_t s = 0; s < report.drivers[i].mayWitness.steps.size(); s++)
            UNIT_CHECK(report2.drivers[j].mayWitness.steps[s].driver
                       == report.drivers[i].mayWitness.steps[s].driver);
    }
}

// SOR drivers are selectable only while a request for them is outstanding.
static void test_sor_request_gating(void)
{
    TreeModel model;
    UModelIndex volume = addVolume(model);

    UModelIndex fileA = simpleDriver(model, volume, 72,  constDepex(true, true)); // SOR
    UModelIndex fileB = simpleDriver(model, volume, 140, constDepex(true));

    AnalysisInput input;
    input.volume = volume;
    input.drivers.push_back(driverWith(fileA, std::vector<EFI_GUID>()));
    input.drivers.push_back(driverWith(fileB, std::vector<EFI_GUID>()));

    // No request: A can never be selected.
    AnalysisReport blocked;
    UNIT_CHECK(analyze(input, blocked));
    NodeIdSet empty;
    IndependentResult expectedBlocked = independentRun(specsFromReport(blocked, empty), GuidSet(), empty);
    checkMayMustAndWitnesses(blocked, expectedBlocked, GuidSet(), empty, empty);
    int a = indexOf(blocked, nodeIdOf(volume, fileA));
    int b = indexOf(blocked, nodeIdOf(volume, fileB));
    UNIT_CHECK(blocked.drivers[a].sor && !blocked.drivers[a].may);
    UNIT_CHECK(blocked.drivers[b].must);
    UNIT_CHECK(blocked.expandedStates == 2);

    // With a request: A participates normally and both drivers are must.
    AnalysisInput requested = input;
    requested.options.sorRequests.insert(nodeIdOf(volume, fileA));
    AnalysisReport granted;
    UNIT_CHECK(analyze(requested, granted));
    IndependentResult expectedGranted = independentRun(specsFromReport(granted, empty), GuidSet(),
                                                       requested.options.sorRequests);
    checkMayMustAndWitnesses(granted, expectedGranted, GuidSet(),
                             requested.options.sorRequests, empty);
    int a2 = indexOf(granted, nodeIdOf(volume, fileA));
    UNIT_CHECK(granted.drivers[a2].may && granted.drivers[a2].must);
}

// Apriori ordering publishes protocols before normal dispatch; the resulting
// report must stay fully readable after the TreeModel has been destroyed.
static void test_apriori_and_model_release(void)
{
    const EFI_GUID guidP = guidFromBytes(0x70);
    const EFI_GUID guidQ = guidFromBytes(0x90);
    NodeId idA, idB, idC;
    AnalysisReport report;
    NodeIdSet aprioriIds;
    GuidSet initial;

    {
        TreeModel model;
        UModelIndex volume = addVolume(model);
        UModelIndex fileA = simpleDriver(model, volume, 72,  pushDepex(guidP, true)); // NOT P -> Q
        UModelIndex fileB = simpleDriver(model, volume, 140, constDepex(true));       // TRUE -> P
        UModelIndex fileC = simpleDriver(model, volume, 208, pushDepex(guidQ));       // Q

        AnalysisInput input;
        input.volume = volume;
        input.drivers.push_back(driverWith(fileA, std::vector<EFI_GUID>(1, guidQ)));
        input.drivers.push_back(driverWith(fileB, std::vector<EFI_GUID>(1, guidP)));
        input.drivers.push_back(driverWith(fileC, std::vector<EFI_GUID>()));
        input.options.aprioriFiles.push_back(fileB);

        UNIT_CHECK(analyze(input, report));
        idA = nodeIdOf(volume, fileA);
        idB = nodeIdOf(volume, fileB);
        idC = nodeIdOf(volume, fileC);
        aprioriIds.insert(idB);
    }
    // TreeModel is gone here; everything below reads only owned report data.

    UNIT_CHECK(report.ok());
    IndependentResult expected = independentRun(specsFromReport(report, aprioriIds), initial, NodeIdSet());
    checkMayMustAndWitnesses(report, expected, initial, NodeIdSet(), aprioriIds);

    int a = indexOf(report, idA);
    int b = indexOf(report, idB);
    int c = indexOf(report, idC);
    UNIT_CHECK(a >= 0 && b >= 0 && c >= 0);
    // B published P first, so NOT P can never open: A and C stay unscheduled.
    UNIT_CHECK(!report.drivers[a].may && !report.drivers[c].may);
    UNIT_CHECK(report.drivers[b].may && report.drivers[b].must);

    const Witness &mayB = report.drivers[b].mayWitness;
    UNIT_CHECK(mayB.present && mayB.steps.size() == 1);
    UNIT_CHECK(mayB.steps[0].apriori);
    UNIT_CHECK(mayB.steps[0].driver == idB);

    // Owned bytes remain valid and re-decodable after model release.
    DecodedDepex redecoded;
    DecodeError error;
    UNIT_CHECK(decode(report.drivers[a].depex.body, redecoded, error));
    UNIT_CHECK(redecoded.operations.size() == report.drivers[a].depex.operations.size());
    UNIT_CHECK(report.drivers[a].depex.body.size() == 19); // PUSH + 16 + NOT + END
    UNIT_CHECK(evaluate(redecoded, initial) == true);  // P absent, NOT P holds
    initial.insert(guidP);
    UNIT_CHECK(evaluate(redecoded, initial) == false); // P present closes the window
}

// --------------------------------------------------------------------------
// Registration
// --------------------------------------------------------------------------

extern "C" const unit_case unit_cases[] = {
    {"not_window_dispatch",              "normal",      test_not_window_dispatch},
    {"duplicate_suppliers_and_initial",  "normal",      test_duplicate_suppliers_and_initial},
    {"cyclic_dependency",                "normal",      test_cyclic_dependency},
    {"constant_false_and_same_guid_copies", "normal",   test_constant_false_and_same_guid_copies},
    {"strict_depex_byte_validation",     "boundary",    test_strict_depex_byte_validation},
    {"selection_and_scope_rejection",    "boundary",    test_selection_and_scope_rejection},
    {"state_space_bounds",               "boundary",    test_state_space_bounds},
    {"nested_encapsulation_and_identities", "integration", test_nested_encapsulation_and_identities},
    {"sor_request_gating",               "integration", test_sor_request_gating},
    {"apriori_and_model_release",        "integration", test_apriori_and_model_release},
};

extern "C" const size_t unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);
