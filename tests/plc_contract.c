/*
 * Registration of the narrowband PLC regression cases.
 *
 * Each case is implemented in plc_model.c against an independent float
 * reference model of the packet-loss path.  This file wires the cases into
 * the shared task harness and asserts their result through TASK_CHECK.
 */
#include <stddef.h>

#include "task_support/check.h"

int plc_case_excitation_single(void);
int plc_case_excitation_twelve(void);
int plc_case_synthesis_single(void);
int plc_case_synthesis_twelve(void);
int plc_case_recovery_single(void);
int plc_case_recovery_twelve(void);

static int run_excitation_single(void)
{
   TASK_CHECK(plc_case_excitation_single() == 0);
   return 0;
}

static int run_excitation_twelve(void)
{
   TASK_CHECK(plc_case_excitation_twelve() == 0);
   return 0;
}

static int run_synthesis_single(void)
{
   TASK_CHECK(plc_case_synthesis_single() == 0);
   return 0;
}

static int run_synthesis_twelve(void)
{
   TASK_CHECK(plc_case_synthesis_twelve() == 0);
   return 0;
}

static int run_recovery_single(void)
{
   TASK_CHECK(plc_case_recovery_single() == 0);
   return 0;
}

static int run_recovery_twelve(void)
{
   TASK_CHECK(plc_case_recovery_twelve() == 0);
   return 0;
}

const struct task_case task_cases[] = {
   {"Excitation", "single_loss_excitation_A",  run_excitation_single},
   {"Excitation", "twelve_loss_excitation_B",  run_excitation_twelve},
   {"Synthesis",  "single_loss_synthesis_A",   run_synthesis_single},
   {"Synthesis",  "twelve_loss_synthesis_B",   run_synthesis_twelve},
   {"Recovery",   "single_loss_recovery_A",    run_recovery_single},
   {"Recovery",   "twelve_loss_recovery_B",    run_recovery_twelve},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
