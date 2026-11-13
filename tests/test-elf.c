#include <yara.h>

#include "blob.h"
#include "util.h"

int main(int argc, char** argv)
{
  int result = 0;

  YR_DEBUG_INITIALIZE();
  YR_DEBUG_FPRINTF(1, stderr, "+ %s() { // in %s\n", __FUNCTION__, argv[0]);

  init_top_srcdir();

  yr_initialize();

  // Each newly exposed e_machine value, checked end to end: patch the value
  // into the ELF header and confirm elf.machine reports it under the new
  // constant. Catches both a wrong constant and a value the module drops.
  struct
  {
    const char* name;
    uint16_t value;
  } machines[] = {
      {"EM_PARISC", 15},
      {"EM_SPARC32PLUS", 18},
      {"EM_S390", 22},
      {"EM_MCORE", 39},
      {"EM_RCE", 39},
      {"EM_SH", 42},
      {"EM_SPARCV9", 43},
      {"EM_ARC_COMPACT", 93},
      {"EM_BPF", 247},
      {"EM_LOONGARCH", 258},
  };

  // e_machine sits at offset 18 and ELF32_FILE is little-endian.
  uint8_t saved_machine[2] = {ELF32_FILE[18], ELF32_FILE[19]};

  for (size_t i = 0; i < sizeof(machines) / sizeof(machines[0]); i++)
  {
    char rule[256];

    ELF32_FILE[18] = (uint8_t) (machines[i].value & 0xFF);
    ELF32_FILE[19] = (uint8_t) (machines[i].value >> 8);

    snprintf(
        rule,
        sizeof(rule),
        "import \"elf\" rule test { condition: elf.machine == elf.%s and "
        "elf.machine == %u }",
        machines[i].name,
        machines[i].value);

    assert_true_rule_blob(rule, ELF32_FILE);
  }

  ELF32_FILE[18] = saved_machine[0];
  ELF32_FILE[19] = saved_machine[1];

  assert_true_rule_blob(
      "import \"elf\" rule test { condition: elf.type }", ELF32_FILE);

  assert_true_rule_blob(
      "import \"elf\" rule test { condition: elf.type }", ELF64_FILE);

  assert_true_rule_blob(
      "import \"elf\" rule test { condition: elf.machine == elf.EM_386 }",
      ELF32_FILE)

      assert_true_rule_blob(
          "import \"elf\" rule test { condition: elf.machine == elf.EM_X86_64 "
          "}",
          ELF64_FILE)

          assert_true_rule_blob(
              "import \"elf\" \
      rule test { \
        strings: $a = { b8 01 00 00 00 bb 2a } \
        condition: $a at elf.entry_point \
      }",
              ELF32_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        strings: $a = { b8 01 00 00 00 bb 2a } \
        condition: $a at elf.entry_point \
      }",
      ELF64_FILE);

  assert_true_rule_blob(
      "import \"elf\" rule test { condition: elf.entry_point == 0xa0 }",
      ELF32_NOSECTIONS);

  assert_true_rule_blob(
      "import \"elf\" rule test { condition: elf.entry_point == 0x1a0 }",
      ELF32_SHAREDOBJ);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: elf.sections[2].name == \".comment\" \
      }",
      ELF64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: elf.machine == elf.EM_MIPS \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.number_of_sections == 35 and elf.number_of_segments == 10 \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.number_of_sections): ( \
            elf.sections[i].type == elf.SHT_PROGBITS and  \
            elf.sections[i].address == 0x400600 and \
            elf.sections[i].name == \".text\") \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
        rule test { \
          condition: \
            for any i in (0..elf.number_of_segments): ( \
            elf.segments[i].type == elf.PT_LOAD and \
            elf.segments[i].virtual_address == 0x00400000 and \
            elf.segments[i].file_size == 0x95c)\
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.dynamic_section_entries == 19 and \
          elf.symtab_entries == 80 \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.symtab_entries): ( \
            elf.symtab[i].shndx == 9 and \
            elf.symtab[i].value == 0x400650 and \
            elf.symtab[i].name == \"_start_c\") \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.symtab[68].name == \"_start_c\" and \
          elf.symtab[68].type == elf.STT_FUNC and \
          elf.symtab[68].bind == elf.STB_GLOBAL and \
          elf.symtab[68].value == 0x400650 and \
          elf.symtab[68].size == 56 \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.dynsym_entries): ( \
            elf.dynsym[i].shndx == 11 and \
            elf.dynsym[i].value == 0x400910 and \
            elf.dynsym[i].name == \"_fini\") \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.dynsym[9].name == \"__RLD_MAP\" and \
          elf.dynsym[9].type == elf.STT_OBJECT and \
          elf.dynsym[9].bind == elf.STB_GLOBAL and \
          elf.dynsym[9].value == 0x411000 and \
          elf.dynsym[9].size == 0 \
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.dynamic[4].type == elf.DT_STRTAB and \
          elf.dynamic[4].val == 0x400484\
      }",
      ELF32_MIPS_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.dynamic_section_entries): ( \
            elf.dynamic[i].type == 0x70000006 and \
            elf.dynamic[i].val == 0x400000)\
      }",
      ELF32_MIPS_FILE);

  // A big-endian symtab whose sh_link is past the real section count must not
  // be parsed against an out-of-range section header. Before the e_shnum byte
  // swap fix a little-endian host accepted the bogus link and reported a
  // phantom symbol named "ABCD".
  assert_false_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.symtab_entries == 1 and elf.symtab[0].name == \"ABCD\" \
      }",
      ELF32_BE_BAD_SYMTAB_LINK);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: elf.machine == elf.EM_X86_64 \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.number_of_sections == 22 and \
          elf.number_of_segments == 7 \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.number_of_sections): ( \
            elf.sections[i].type == elf.SHT_PROGBITS and \
            elf.sections[i].address == 0x601000 and \
            elf.sections[i].name == \".got.plt\") \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
         condition: \
            for any i in (0..elf.number_of_segments): ( \
              elf.segments[i].type == elf.PT_LOAD and \
              elf.segments[i].virtual_address == 0x600e78 and \
              elf.segments[i].file_size == 0x1b0) \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
         condition: \
            elf.dynamic_section_entries == 18 and \
            elf.symtab_entries == 48  \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.symtab_entries): ( \
            elf.symtab[i].shndx == 8 and \
            elf.symtab[i].value == 0x400400 and \
            elf.symtab[i].name == \"main\") \
     }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.symtab[20].name == \"__JCR_LIST__\" and \
          elf.symtab[20].type == elf.STT_OBJECT and \
          elf.symtab[20].bind == elf.STB_LOCAL and \
          elf.symtab[20].value == 0x600e88 and \
          elf.symtab[20].size == 0 \
      }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.dynamic[13].type == elf.DT_PLTGOT and \
          elf.dynamic[13].val == 0x601000 \
     }",
      ELF_x64_FILE);

  assert_true_rule_blob(
      "import \"elf\" \
      rule test { \
        condition: \
          for any i in (0..elf.dynamic_section_entries): ( \
            elf.dynamic[i].type == elf.DT_JMPREL and \
            elf.dynamic[i].val == 0x4003c0) \
      }",
      ELF_x64_FILE);

  assert_true_rule_file(
      "import \"elf\" \
      rule test { \
        condition: \
          elf.telfhash() == \
            \"T174B012188204F00184540770331E0B111373086019509C464D0ACE88181266C09774FA\" \
      }",
      "tests/data/elf_with_imports");


  yr_finalize();

  YR_DEBUG_FPRINTF(
      1, stderr, "} = %d // %s() in %s\n", result, __FUNCTION__, argv[0]);

  return result;
}
