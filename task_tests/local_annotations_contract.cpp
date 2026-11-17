// Copyright 2000-2024 JetBrains s.r.o. and contributors. Use of this source code is governed by the Apache 2.0 license.
//
// Contract tests for Code-level local variable type annotations. The driver only invokes JDK
// tools (javac/java) on benign fixtures and the production read-only API; fixture classes are
// never loaded or executed.
#include "task_support/check.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const char* kClasses = ".task-build/classes";
const char* kFixtureDir = "task_support/fixtures";
const char* kProbe = "LocalAnnotationProbe";
const char* kDecompiler = "org.jetbrains.java.decompiler.main.decompiler.ConsoleDecompiler";

std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'') quoted += "'\\''";
    else quoted += c;
  }
  quoted += '\'';
  return quoted;
}

int runCapture(const std::string& command, std::string* output) {
  std::string full = command + " 2>&1";
  FILE* pipe = popen(full.c_str(), "r");
  if (pipe == NULL) {
    return -1;
  }
  char buffer[512];
  size_t read;
  while ((read = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output->append(buffer, read);
  }
  int status = pclose(pipe);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runQuiet(const std::string& command) {
  std::string output;
  return runCapture(command, &output);
}

bool makeDir(const std::string& path) {
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string scratchDir() {
  const char* tmp = getenv("TMPDIR");
  std::string base = (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp";
  std::string pattern = base + "/local_ann_XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  char* created = mkdtemp(buffer.data());
  return created == NULL ? std::string() : std::string(created);
}

bool compileSources(const std::vector<std::string>& sources,
                    const std::string& outDir,
                    bool withDebugInfo) {
  std::string command = "javac --release 21 -proc:none -encoding UTF-8 ";
  command += withDebugInfo ? "-g " : "-g:none ";
  command += "-d " + shellQuote(outDir) + " ";
  for (const std::string& source : sources) {
    command += shellQuote(source) + " ";
  }
  return runQuiet(command) == 0;
}

bool runProbe(const std::string& classDir, std::string* output) {
  std::string command = "java -cp " + std::string(kClasses) + " " + kProbe +
                        " attrs " + shellQuote(classDir);
  return runCapture(command, output) == 0;
}

bool decompile(const std::string& classFile, const std::string& outDir) {
  std::string command = "java -cp " + std::string(kClasses) + " " + kDecompiler +
                        " -dgs=1 -log=WARN " + shellQuote(classFile) + " " + shellQuote(outDir);
  return runQuiet(command) == 0;
}

bool readFile(const std::string& path, std::string* content) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == NULL) {
    return false;
  }
  char buffer[512];
  size_t read;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content->append(buffer, read);
  }
  fclose(file);
  return true;
}

int countOccurrences(const std::string& text, const std::string& needle) {
  int count = 0;
  size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Splits a probe output line into space separated tokens. Fixture annotations contain no spaces.
std::vector<std::string> tokens(const std::string& line) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start < line.size()) {
    size_t end = line.find(' ', start);
    if (end == std::string::npos) end = line.size();
    if (end > start) result.push_back(line.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

struct ProbeDump {
  std::string raw;
  std::vector<std::vector<std::string> > local;
  std::vector<std::vector<std::string> > methodAttrs;
  std::vector<std::vector<std::string> > codeAttrs;
};

bool loadProbe(const std::string& classDir, ProbeDump* dump) {
  if (!runProbe(classDir, &dump->raw)) {
    return false;
  }
  size_t start = 0;
  while (start < dump->raw.size()) {
    size_t end = dump->raw.find('\n', start);
    if (end == std::string::npos) end = dump->raw.size();
    std::string line = dump->raw.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> parts = tokens(line);
    if (parts.empty()) continue;
    if (parts[0] == "LOCAL") dump->local.push_back(parts);
    else if (parts[0] == "MATTR") dump->methodAttrs.push_back(parts);
    else if (parts[0] == "CATTR") dump->codeAttrs.push_back(parts);
  }
  return true;
}

bool hasLocalAnnotation(const ProbeDump& dump,
                        const std::string& visibility,
                        const std::string& annotation,
                        const std::string& path) {
  for (const auto& line : dump.local) {
    // LOCAL <V|I> <slot> <startPc> <length> <path> <annotation>
    if (line.size() == 7 && line[1] == visibility && line[5] == path && line[6] == annotation) {
      return true;
    }
  }
  return false;
}

int countLocalLines(const ProbeDump& dump, const std::string& annotation) {
  int count = 0;
  for (const auto& line : dump.local) {
    if (line.size() == 7 && line[6] == annotation) ++count;
  }
  return count;
}

bool hasMethodAttr(const ProbeDump& dump,
                   const std::string& visibility,
                   int targetType,
                   const std::string& annotation) {
  for (const auto& line : dump.methodAttrs) {
    // MATTR <V|I> <target type> <annotation>
    if (line.size() == 4 && line[1] == visibility &&
        std::atoi(line[2].c_str()) == targetType && line[3] == annotation) {
      return true;
    }
  }
  return false;
}

bool hasCodeAttr(const ProbeDump& dump,
                 const std::string& visibility,
                 int targetType) {
  for (const auto& line : dump.codeAttrs) {
    // CATTR <V|I> <target type> <annotation>
    if (line.size() == 4 && line[1] == visibility &&
        std::atoi(line[2].c_str()) == targetType) {
      return true;
    }
  }
  return false;
}

bool sameSlotDistinctRanges(const ProbeDump& dump,
                            const std::string& first,
                            const std::string& second) {
  int firstSlot = -1;
  int firstStart = -1;
  int secondStart = -1;
  for (const auto& line : dump.local) {
    if (line.size() != 7) continue;
    int slot = std::atoi(line[2].c_str());
    int startPc = std::atoi(line[3].c_str());
    if (line[6] == first) {
      if (firstSlot == -1) firstSlot = slot;
      if (firstStart == -1) firstStart = startPc;
    }
    if (line[6] == second) {
      if (firstSlot != -1 && slot != firstSlot) return false;
      if (secondStart == -1) secondStart = startPc;
    }
  }
  return firstSlot != -1 && firstStart != -1 && secondStart != -1 && firstStart != secondStart;
}

std::string fixture(const std::string& name) {
  return std::string(kFixtureDir) + "/" + name;
}

// ---------------------------------------------------------------------------
// lifetimes
// ---------------------------------------------------------------------------

int lifetimesSlotReuseNoLvt() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"first\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"second\")", "-"));
  TASK_CHECK(sameSlotDistinctRanges(dump, "@Mark(\"first\")", "@Mark(\"second\")"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Locals.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Locals.java", &source));
  TASK_CHECK(countOccurrences(source, "@Mark(\"first\")") == 1);
  TASK_CHECK(countOccurrences(source, "@Mark(\"second\")") == 1);

  std::string recompiled = scratch + "/re";
  TASK_CHECK(makeDir(recompiled));
  std::string recompile = "javac --release 21 -proc:none -cp " + shellQuote(scratch) +
                          " -d " + shellQuote(recompiled) + " " + shellQuote(out + "/Locals.java");
  TASK_CHECK(runQuiet(recompile) == 0);
  return 0;
}

int lifetimesInvisibleAnnotations() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"invisible-one\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"invisible-two\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"hidden-one\")", "-"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Locals.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Locals.java", &source));
  TASK_CHECK(contains(source, "@Hidden(\"invisible-one\")"));
  TASK_CHECK(contains(source, "@Hidden(\"invisible-two\")"));
  return 0;
}

int lifetimesSplitRangeOneDeclaration() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  int ranges = countLocalLines(dump, "@Mark(\"split\")");
  TASK_CHECK(ranges >= 1);

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Locals.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Locals.java", &source));
  // One annotation spanning one or several ranges must still produce a single declaration.
  TASK_CHECK(countOccurrences(source, "@Mark(\"split\")") == 1);
  return 0;
}

int lifetimesAliasNotInlined() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Aliases.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"alias\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"second\")", "-"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Aliases.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Aliases.java", &source));
  TASK_CHECK(contains(source, "@Mark(\"alias\")"));
  TASK_CHECK(contains(source, "@Mark(\"second\")"));
  return 0;
}

int lifetimesMethodAndCodeCoexist() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  // 0x14 = METHOD_RETURN_TYPE, 0x16 = METHOD_PARAMETER, 0x40 = LOCAL_VARIABLE
  TASK_CHECK(hasMethodAttr(dump, "V", 0x14, "@Mark(\"return\")"));
  TASK_CHECK(hasMethodAttr(dump, "V", 0x16, "@Mark(\"parameter\")"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"first\")", "-"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Locals.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Locals.java", &source));
  TASK_CHECK(contains(source, "@Mark(\"return\")"));
  TASK_CHECK(contains(source, "@Mark(\"parameter\")"));
  TASK_CHECK(contains(source, "@Mark(\"first\")"));
  return 0;
}

int lifetimesNonLocalTargetsKept() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Targets.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  // 0x41 = RESOURCE_VARIABLE, 0x42 = CATCH_CLAUSE are stable Code-level targets. The instanceof
  // annotation is reported either as an expression target (0x43) or, for pattern bindings, as a
  // local variable target, so only the two unambiguous targets are asserted here.
  TASK_CHECK(hasCodeAttr(dump, "V", 0x41));
  TASK_CHECK(hasCodeAttr(dump, "V", 0x42));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Targets.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Targets.java", &source));
  TASK_CHECK(contains(source, "class Targets"));
  std::string recompiled = out + "/re";
  TASK_CHECK(makeDir(recompiled));
  TASK_CHECK(runQuiet(("javac --release 21 -proc:none -cp " + shellQuote(scratch) +
                       " -d " + shellQuote(recompiled) + " " + shellQuote(out + "/Targets.java")).c_str()) == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

int pathsArrayDimensions() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Arrays.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  // Outer array type has an empty type path; each ARRAY entry navigates one component deeper.
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"array-2d\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"array-1d\")", "0"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"element\")", "0.0"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Arrays.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Arrays.java", &source));
  TASK_CHECK(contains(source, "@Mark(\"element\") String"));
  TASK_CHECK(contains(source, "@Hidden(\"array-2d\") []"));
  TASK_CHECK(contains(source, "@Mark(\"array-1d\") []"));
  TASK_CHECK(source.find("@Mark(\"element\") String") < source.find("@Hidden(\"array-2d\") []"));
  TASK_CHECK(source.find("@Hidden(\"array-2d\") []") < source.find("@Mark(\"array-1d\") []"));

  std::string recompiled = scratch + "/re";
  TASK_CHECK(makeDir(recompiled));
  std::string recompile = "javac --release 21 -proc:none -cp " + shellQuote(scratch) +
                          " -d " + shellQuote(recompiled) + " " + shellQuote(out + "/Arrays.java");
  TASK_CHECK(runQuiet(recompile) == 0);
  return 0;
}

int pathsEmptyAndSingleArray() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Arrays.java")}, scratch, false));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"element-one\")", "0"));
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"array-one\")", "-"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Arrays.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Arrays.java", &source));
  TASK_CHECK(contains(source, "@Mark(\"element-one\") String @Hidden(\"array-one\") []"));
  return 0;
}

// ---------------------------------------------------------------------------
// compatibility
// ---------------------------------------------------------------------------

int compatibilityWithLvt() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, true));

  ProbeDump dump;
  TASK_CHECK(loadProbe(scratch, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"first\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"second\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"invisible-one\")", "-"));

  std::string out = scratch + "/out";
  TASK_CHECK(makeDir(out));
  TASK_CHECK(decompile(scratch + "/Locals.class", out));
  std::string source;
  TASK_CHECK(readFile(out + "/Locals.java", &source));
  TASK_CHECK(countOccurrences(source, "@Mark(\"first\")") == 1);
  TASK_CHECK(countOccurrences(source, "@Mark(\"second\")") == 1);

  std::string recompiled = scratch + "/re";
  TASK_CHECK(makeDir(recompiled));
  std::string recompile = "javac --release 21 -proc:none -cp " + shellQuote(scratch) +
                          " -d " + shellQuote(recompiled) + " " + shellQuote(out + "/Locals.java");
  TASK_CHECK(runQuiet(recompile) == 0);
  return 0;
}

int compatibilityRoundTrip() {
  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(compileSources({fixture("Marks.java"), fixture("Locals.java")}, scratch, false));

  std::string first = scratch + "/first";
  TASK_CHECK(makeDir(first));
  TASK_CHECK(decompile(scratch + "/Locals.class", first));

  std::string second = scratch + "/second";
  TASK_CHECK(makeDir(second));
  std::string recompile = "javac --release 21 -proc:none -cp " + shellQuote(scratch) +
                          " -d " + shellQuote(second) + " " + shellQuote(first + "/Locals.java");
  TASK_CHECK(runQuiet(recompile) == 0);

  ProbeDump dump;
  TASK_CHECK(loadProbe(second, &dump));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"first\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "V", "@Mark(\"second\")", "-"));
  TASK_CHECK(hasLocalAnnotation(dump, "I", "@Hidden(\"invisible-one\")", "-"));

  std::string secondOut = scratch + "/second-out";
  TASK_CHECK(makeDir(secondOut));
  TASK_CHECK(decompile(second + "/Locals.class", secondOut));
  std::string source;
  TASK_CHECK(readFile(secondOut + "/Locals.java", &source));
  TASK_CHECK(contains(source, "@Mark(\"first\")"));
  TASK_CHECK(contains(source, "@Mark(\"second\")"));
  return 0;
}

int compatibilityPlainControl() {
  const char* plainClass = ".task-build/fixtures/Plain.class";
  struct stat st;
  TASK_CHECK(stat(plainClass, &st) == 0);

  std::string scratch = scratchDir();
  TASK_CHECK(!scratch.empty());
  TASK_CHECK(decompile(plainClass, scratch));

  std::string source;
  TASK_CHECK(readFile(scratch + "/Plain.java", &source));
  TASK_CHECK(contains(source, "class Plain"));
  TASK_CHECK(contains(source, " + 1;"));
  TASK_CHECK(!contains(source, "@Mark"));
  TASK_CHECK(!contains(source, "@Hidden"));

  std::string recompiled = scratch + "/re";
  TASK_CHECK(makeDir(recompiled));
  std::string recompile = "javac --release 21 -proc:none -d " + shellQuote(recompiled) +
                          " " + shellQuote(scratch + "/Plain.java");
  TASK_CHECK(runQuiet(recompile) == 0);
  return 0;
}

}  // namespace

extern const struct task_case task_cases[] = {
  {"lifetimes", "slot_reuse_no_lvt", lifetimesSlotReuseNoLvt},
  {"lifetimes", "invisible_annotations", lifetimesInvisibleAnnotations},
  {"lifetimes", "split_range_one_declaration", lifetimesSplitRangeOneDeclaration},
  {"lifetimes", "alias_not_inlined", lifetimesAliasNotInlined},
  {"lifetimes", "method_and_code_coexist", lifetimesMethodAndCodeCoexist},
  {"lifetimes", "non_local_targets_kept", lifetimesNonLocalTargetsKept},
  {"paths", "array_dimensions", pathsArrayDimensions},
  {"paths", "empty_and_single_array", pathsEmptyAndSingleArray},
  {"compatibility", "with_lvt", compatibilityWithLvt},
  {"compatibility", "round_trip", compatibilityRoundTrip},
  {"compatibility", "plain_control", compatibilityPlainControl},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
