/* depex_offline_example.cpp

Copyright (c) 2026, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

// Offline DXE dispatch analysis example.
//
// Everything here is plain C++ plus the in-tree common/ library: no Qt, no
// firmware image on disk, no PE machine code parsing and no driver loading.
// The TreeModel and every DXE_DEPEX byte stream are generated below, and the
// protocol GUIDs each driver publishes are explicit analysis assumptions
// handed to Depex::analyze().
//
// Scenario (the NOT-window example from the module documentation):
//   driver A depends on NOT P and publishes Q
//   driver B depends on TRUE     and publishes P
//   driver C depends on Q
// If A runs first all three drivers dispatch; if B runs first the NOT P
// window closes and A and C stay unscheduled. B is therefore "must", while A
// and C are only "may".

#include <cstdio>
#include <cstring>
#include <vector>

#include "common/treemodel.h"
#include "common/types.h"
#include "common/ffs.h"
#include "common/depex.h"

using namespace Depex;

static EFI_GUID makeGuid(unsigned char first)
{
    unsigned char raw[16];
    for (int i = 0; i < 16; i++)
        raw[i] = (unsigned char)(first + (unsigned char)i);
    EFI_GUID guid;
    memcpy(&guid, raw, sizeof(guid));
    return guid;
}

static UByteArray pushGuidDepex(const EFI_GUID &guid, bool negate)
{
    std::vector<unsigned char> bytes;
    bytes.push_back(EFI_DEP_PUSH);
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(&guid);
    for (int i = 0; i < 16; i++)
        bytes.push_back(raw[i]);
    if (negate)
        bytes.push_back(EFI_DEP_NOT);
    bytes.push_back(EFI_DEP_END);
    return UByteArray(reinterpret_cast<const char *>(&bytes[0]), (int32_t)bytes.size());
}

static UByteArray trueDepex()
{
    const unsigned char bytes[] = {EFI_DEP_TRUE, EFI_DEP_END};
    return UByteArray(reinterpret_cast<const char *>(bytes), 2);
}

static UModelIndex makeDriver(TreeModel &model, const UModelIndex &volume,
                              UINT32 offset, const UByteArray &depex)
{
    UModelIndex file = model.addItem(offset, Types::File, EFI_FV_FILETYPE_DRIVER,
                                     "Driver", "", "", UByteArray(24, 0),
                                     UByteArray(), UByteArray(), Fixed, volume);
    model.addItem(24, Types::Section, EFI_SECTION_DXE_DEPEX, "Depex", "", "",
                  UByteArray(4, 0), depex, UByteArray(), Fixed, file);
    return file;
}

static void printGuidSet(const char *label, const GuidSet &protocols)
{
    std::printf("    %s:", label);
    if (protocols.empty()) {
        std::printf(" (none)\n");
        return;
    }
    for (GuidSet::const_iterator it = protocols.begin(); it != protocols.end(); ++it)
        std::printf(" %s", guidToUString(*it, false).toLocal8Bit());
    std::printf("\n");
}

static void printWitness(const char *label, const Witness &witness)
{
    if (!witness.present) {
        std::printf("  %s: no such witness\n", label);
        return;
    }
    if (witness.steps.empty()) {
        std::printf("  %s: present but empty maximal schedule\n", label);
        return;
    }
    std::printf("  %s:\n", label);
    for (size_t i = 0; i < witness.steps.size(); i++) {
        const WitnessStep &step = witness.steps[i];
        std::printf("    %zu. %s%s expression=%s\n",
                    i + 1,
                    step.driver.toString().toLocal8Bit(),
                    step.apriori ? " (apriori)" : "",
                    step.expressionResult ? "true" : "false");
        printGuidSet("protocols installed when chosen", step.satisfiedProtocols);
    }
}

int main()
{
    const EFI_GUID guidP = makeGuid(0x10);
    const EFI_GUID guidQ = makeGuid(0x30);

    // 1. Build the firmware volume and the selected driver nodes in C++.
    TreeModel model;
    UModelIndex volume = model.addItem(0, Types::Volume, Subtypes::Ffs2Volume,
                                       "FV", "", "", UByteArray(72, 0),
                                       UByteArray(), UByteArray(), Fixed);
    UModelIndex fileA = makeDriver(model, volume, 72,  pushGuidDepex(guidP, true)); // NOT P
    UModelIndex fileB = makeDriver(model, volume, 140, trueDepex());                // TRUE
    UModelIndex fileC = makeDriver(model, volume, 208, pushGuidDepex(guidQ, false)); // Q

    // 2. State the analysis assumptions: nothing is installed initially, and
    //    the protocol publication lists come from the caller, not the PE code.
    AnalysisInput input;
    input.volume = volume;

    DriverInput driverA;
    driverA.file = fileA;
    driverA.protocols.push_back(guidQ);
    input.drivers.push_back(driverA);

    DriverInput driverB;
    driverB.file = fileB;
    driverB.protocols.push_back(guidP);
    input.drivers.push_back(driverB);

    DriverInput driverC;
    driverC.file = fileC;
    input.drivers.push_back(driverC);

    // 3. Exhaustive offline analysis over executed-driver sets.
    AnalysisReport report;
    if (!analyze(input, report)) {
        std::printf("analysis failed: %s (%s)\n",
                    report.errorMessage.toLocal8Bit(),
                    report.errorIdentity.toLocal8Bit());
        return 1;
    }

    std::printf("expanded states: %zu, transitions: %zu\n\n",
                report.expandedStates, report.transitions);

    for (size_t i = 0; i < report.drivers.size(); i++) {
        const DriverReport &dr = report.drivers[i];
        std::printf("%s  may=%d must=%d sor=%d\n",
                    dr.identity.toString().toLocal8Bit(),
                    dr.may, dr.must, dr.sor);
        printWitness("witness that executes it", dr.mayWitness);
        printWitness("witness that excludes it", dr.withoutWitness);
        std::printf("\n");
    }

    return 0;
}
