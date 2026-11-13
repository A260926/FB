check_PROGRAMS = ltcencode ltcdecode ltcloop stream_encoder

CLEANFILES = output.raw atconfig

EXTRA_DIST= \
	example_encode.c \
	timecode.raw timecode.txt \
	expect_48k_2sec.txt expect_96k_2sec.txt \
	../support/test_main.c ../support/test_support.h

LIBLTCDIR =../src
INCLUDES = -I$(srcdir)/$(LIBLTCDIR)

ltcdecode_SOURCES = ltcdecode.c
ltcdecode_LDADD = $(LIBLTCDIR)/libltc.la -lm
ltcdecode_CFLAGS=-g -Wall

ltcencode_SOURCES = ltcencode.c
ltcencode_CFLAGS=-g -Wall
ltcencode_LDADD = $(LIBLTCDIR)/libltc.la -lm

ltcloop_SOURCES = ltcloop.c
ltcloop_CFLAGS=-g -Wall
ltcloop_LDADD = $(LIBLTCDIR)/libltc.la -lm

stream_encoder_SOURCES = stream_encoder.c ../support/test_main.c
stream_encoder_CFLAGS=-g -Wall -I$(srcdir)/../support
stream_encoder_LDADD = $(LIBLTCDIR)/libltc.la -lm


check: $(check_PROGRAMS)
	 date
	 uname -a
	 @echo "-----------------------------------------------------------------"
	 ./ltcencode output.raw
	 ./ltcdecode output.raw | diff -q $(srcdir)/expect_48k_2sec.txt -
	 @echo "-----------------------------------------------------------------"
	 ./ltcencode output.raw 192000
	 ./ltcdecode output.raw 7680 | diff -q $(srcdir)/expect_96k_2sec.txt -
	 @echo "-----------------------------------------------------------------"
	 ./ltcdecode $(srcdir)/timecode.raw 882 | diff -q $(srcdir)/timecode.txt -
	 @echo "-----------------------------------------------------------------"
	 ./ltcloop
	 @echo "-----------------------------------------------------------------"
	 ./stream_encoder
	 @echo "-----------------------------------------------------------------"
	 @echo "  ${PACKAGE}-${VERSION} passed all tests."
	 @echo "-----------------------------------------------------------------"
