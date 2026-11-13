/* depex.h

Copyright (c) 2026, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

// Offline DXE dependency (DXE_DEPEX) decoder and dispatch analyzer.
//
// The module never loads or executes drivers and never inspects PE machine
// code. The protocol GUIDs a driver publishes are explicit analysis
// assumptions handed in by the caller. It has no Qt dependency and works with
// the non-Qt TreeModel implementation as well as the Qt one.

#ifndef DEPEX_H
#define DEPEX_H

#include <cstring>
#include <set>
#include <vector>

#include "basetypes.h"
#include "ubytearray.h"
#include "ustring.h"
#include "treemodel.h"

namespace Depex {

// GUIDs are compared by their raw 16 on-disk bytes, never by the fields of
// the EFI_GUID struct.
struct GuidLess {
    bool operator()(const EFI_GUID &lhs, const EFI_GUID &rhs) const {
        return memcmp(&lhs, &rhs, sizeof(EFI_GUID)) < 0;
    }
};

typedef std::set<EFI_GUID, GuidLess> GuidSet;

// --------------------------------------------------------------------------
// Strict decoder
// --------------------------------------------------------------------------

enum OperationKind {
    OpPush = 0,
    OpTrue,
    OpFalse,
    OpAnd,
    OpOr,
    OpNot
};

struct Operation {
    UINT8       opcode;   // Raw EFI_DEP_* opcode byte
    OperationKind kind;
    UINT32      offset;   // Opcode offset relative to the DEPEX body start
    EFI_GUID    guid;     // Raw on-disk bytes, valid only for OpPush
};

struct DecodeError {
    UString message;
    UINT32  offset;       // First error offset relative to the DEPEX body
    DecodeError() : offset(0) {}
};

// A reusable, strictly validated boolean expression. The object owns a copy
// of the source bytes, so it stays readable after the TreeModel is gone.
struct DecodedDepex {
    bool                     sor;
    std::vector<Operation>   operations;
    UByteArray               body;

    DecodedDepex() : sor(false) {}
};

// Decode a DXE_DEPEX body strictly. The whole byte stream is validated,
// including branches whose boolean outcome would already be decided while
// evaluating left to right. Returns false and fills error on any malformed
// stream. BEFORE/AFTER opcodes and anything but a DXE dependency are rejected
// here instead of being silently treated as TRUE.
bool decode(const UByteArray &body, DecodedDepex &out, DecodeError &error);

// Historical one-opcode-per-line listing, in the exact textual order the
// FfsParser used to emit ("SOR", "PUSH <guid>", "AND", ..., "END"). Both the
// parser and the analyzer render through this single function.
UString renderText(const DecodedDepex &decoded);

// Evaluate a decoded expression against an installed-protocol set. PUSH
// compares GUIDs by their raw 16 bytes.
bool evaluate(const DecodedDepex &decoded, const GuidSet &installed);

// --------------------------------------------------------------------------
// Node identity: firmware-volume-relative path plus offset. Two file copies
// are never merged through their file GUID.
// --------------------------------------------------------------------------

struct NodeId {
    UString path;   // e.g. "/File@72/Section@24/Section@40"
    UINT32  base;   // FV-relative base offset

    NodeId() : base(0) {}
    NodeId(const UString &aPath, UINT32 aBase) : path(aPath), base(aBase) {}

    UString toString() const;

    bool operator==(const NodeId &other) const { return path == other.path && base == other.base; }
    bool operator!=(const NodeId &other) const { return !(*this == other); }
};

struct NodeIdLess {
    bool operator()(const NodeId &lhs, const NodeId &rhs) const {
        if (lhs.path < rhs.path)
            return true;
        if (rhs.path < lhs.path)
            return false;
        return lhs.base < rhs.base;
    }
};

typedef std::set<NodeId, NodeIdLess> NodeIdSet;

// Build the FV-relative identity of any node below (or equal to) volume.
NodeId nodeIdOf(const UModelIndex &volume, const UModelIndex &node);

// --------------------------------------------------------------------------
// Analysis input
// --------------------------------------------------------------------------

struct DriverInput {
    UModelIndex              file;       // Selected driver file node
    std::vector<EFI_GUID>    protocols;  // Published after successful dispatch
};

struct AnalysisOptions {
    // Optional apriori list, in apriori order. Every entry must reference a
    // selected file; duplicates are rejected.
    std::vector<UModelIndex> aprioriFiles;
    // Optional SOR request set. A driver carrying a leading SOR opcode is
    // only selectable while its identity is member of this set.
    NodeIdSet                sorRequests;
};

struct AnalysisInput {
    UModelIndex                    volume;
    std::vector<DriverInput>       drivers;
    GuidSet                        initialProtocols;
    AnalysisOptions                options;
};

// --------------------------------------------------------------------------
// Analysis output (fully self-owned, valid after the model is released)
// --------------------------------------------------------------------------

struct WitnessStep {
    NodeId  driver;
    bool    apriori;          // Executed from the apriori list, deps ignored
    bool    expressionResult; // Expression value against the protocols below
    GuidSet satisfiedProtocols; // Protocols installed when the step was chosen
};

struct Witness {
    // present == false means "no such witness" and must not be confused with
    // a present but empty schedule (a maximal schedule executing nobody).
    bool                          present;
    std::vector<WitnessStep>      steps;

    Witness() : present(false) {}
};

struct DriverReport {
    NodeId          identity;
    NodeId          depexSection;                // Where the bytes came from
    bool            sor;
    DecodedDepex    depex;                       // Owned decoded bytes/expression
    std::vector<EFI_GUID> publishedProtocols;    // Owned copy of the assumption
    bool            may;                         // Some maximal schedule runs it
    bool            must;                        // Every maximal schedule runs it
    Witness         mayWitness;                  // Present iff may
    Witness         withoutWitness;              // Present iff !must
};

struct AnalysisReport {
    std::vector<DriverReport> drivers; // Normalized by (path, base)
    size_t                    expandedStates;
    size_t                    transitions;

    // Error reporting. errorHasOffset is false for validation failures that
    // are not tied to a DEPEX body offset.
    UString errorMessage;
    UString errorIdentity;
    UINT32  errorOffset;
    bool    errorHasOffset;

    AnalysisReport() : expandedStates(0), transitions(0), errorOffset(0), errorHasOffset(false) {}
    bool ok() const { return errorMessage.isEmpty(); }
};

// Run the exhaustive offline analysis. On failure the report only carries
// the error description (identity and, where applicable, DEPEX body offset).
bool analyze(const AnalysisInput &input, AnalysisReport &report);

} // namespace Depex

#endif // DEPEX_H
