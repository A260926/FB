###############################################################################
#
# Makefile for libxlsxwriter functional test executables.
#
# SPDX-License-Identifier: BSD-2-Clause
# Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
#

# Keep the output quiet by default.
Q=@
ifdef V
Q=
endif


# Directory variables.
INC_DIR     = ../../../include

# Flags passed to the C++ compiler.
CFLAGS += -g -Wall -Wextra

# Source files to compile.
SRCTESTFILES ?= *.c
SRCS = $(wildcard $(SRCTESTFILES))
EXES = $(patsubst %.c,%,$(SRCS))


ifndef GCOV
LIBXLSXWRITER = ../../../src/libxlsxwriter.a
else
# Link against the same lib used by the unit tests.
LIBXLSXWRITER = ../../../src/libxlsxwriter_test.a
endif


LIBS = $(LIBXLSXWRITER) -lz
ifdef USE_SYSTEM_MINIZIP
LIBS   += -lminizip
CFLAGS += -DUSE_SYSTEM_MINIZIP
endif

ifdef USE_OPENSSL_MD5
LIBS   += -lcrypto
endif

# Use a third party double number formatting function.
ifdef USE_DTOA_LIBRARY
CFLAGS += -DUSE_DTOA_LIBRARY
endif

all : $(LIBXLSXWRITER) $(EXES)

$(LIBXLSXWRITER):
ifndef USE_SYSTEM_MINIZIP
	$(Q)$(MAKE) -C ../../../third_party/minizip
endif
	$(Q)$(MAKE) -C ../../../src

clean :
	$(Q)rm -f $(EXES)
	$(Q)rm -f *.fo functional_all functional_main.c
	$(Q)rm -rf *.dSYM *.gcno *.gcda

# Executable targets.
%: %.c $(LIBXLSXWRITER)
	$(Q)$(CC) -I$(INC_DIR) $(CFLAGS) $(LDFLAGS) $(GCOV) $< -o $@ $(LIBS)

###############################################################################
#
# Fast combined test executable.
#
# The rules above build one executable per test, which means one (slow) link
# against the static library per test. The rules below compile each
# test's main() under a unique name (-Dmain=<testname>) and link all of them
# into a single "functional_all" executable with a generated dispatcher. This
# needs only one link step for the whole (sub)set of tests, which is much
# faster to build. Run a single test with: ./functional_all <test_name>
#
# This requires that test files don't define global symbols other than main().
FOBJS = $(patsubst %.c,%.fo,$(SRCS))

%.fo : %.c $(LIBXLSXWRITER)
	$(Q)$(CC) -I$(INC_DIR) $(CFLAGS) $(GCOV) -Dmain=$* -c $< -o $@

functional_all : $(FOBJS) $(LIBXLSXWRITER)
	$(Q)( echo '#include <string.h>'; \
	      echo '#include <stdio.h>'; \
	      for o in $(FOBJS); do b=$${o%.fo}; echo "extern int $$b(void);"; done; \
	      echo 'int main(int argc, char **argv) {'; \
	      echo '    if (argc < 2) return 2;'; \
	      for o in $(FOBJS); do b=$${o%.fo}; \
	          echo "    if (!strcmp(argv[1], \"$$b\")) return $$b();"; done; \
	      echo '    fprintf(stderr, "functional_all: unknown test: %s", argv[1]);'; \
	      echo '    return 2;'; \
	      echo '}' ) > functional_main.c
	$(Q)$(CC) -I$(INC_DIR) $(CFLAGS) $(LDFLAGS) $(GCOV) $(FOBJS) functional_main.c -o $@ $(LIBS)

test_valgrind: all
	$(Q)$(foreach exe,$(EXES),valgrind -q --error-exitcode=1 --leak-check=full ./$(exe) || exit;)
