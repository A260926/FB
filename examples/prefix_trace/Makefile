#
# Prefix tracing example
#
# Builds a small console program that shows the prefix side-band produced by
# distorm_decompose_prefix_trace(). Run from this directory or the repo root.
#

CC	= cc
CFLAGS	= -std=c99 -O1 -Wall -I../../include -I../../src
SRCS	= ../../src/mnemonics.c ../../src/textdefs.c ../../src/prefix.c ../../src/prefix_trace.c \
	  ../../src/operands.c ../../src/insts.c ../../src/instructions.c ../../src/distorm.c ../../src/decoder.c

all: prefix_trace_demo

prefix_trace_demo: main.c
	${CC} ${CFLAGS} ${SRCS} main.c -o $@

clean:
	/bin/rm -f prefix_trace_demo
