# alacconvert make

CFLAGS = -g -O3 -c
LFLAGS = -Wall -L$(CODECDIR) -lalac
CC = g++

SRCDIR = .
CODECDIR = ../codec
OBJDIR = ./obj
INCLUDES = ../codec

HEADERS = \
$(SRCDIR)/CAFFileALAC.h \
$(SRCDIR)/ALACProjectChannels.h \
$(CODECDIR)/EndianPortable.h \
$(CODECDIR)/aglib.h \
$(CODECDIR)/ALACAudioTypes.h \
$(CODECDIR)/ALACBitUtilities.h\
$(CODECDIR)/ALACDecoder.h \
$(CODECDIR)/ALACEncoder.h \
$(CODECDIR)/dplib.h \
$(CODECDIR)/matrixlib.h

SOURCES = \
$(SRCDIR)/main.cpp \
$(SRCDIR)/CAFFileALAC.cpp \
$(SRCDIR)/ALACProjectChannels.cpp

OBJS = \
main.o \
CAFFileALAC.o \
ALACProjectChannels.o

alacconvert:	$(OBJS)
	(cd $(CODECDIR); $(MAKE))
	$(CC) $(LFLAGS) $(OBJS) -o alacconvert

main.o : main.cpp
	$(CC) -I $(INCLUDES) $(CFLAGS) main.cpp

CAFFileALAC.o : CAFFileALAC.cpp
	$(CC) -I $(INCLUDES) $(CFLAGS) CAFFileALAC.cpp

ALACProjectChannels.o : ALACProjectChannels.cpp
	$(CC) -I $(INCLUDES) $(CFLAGS) ALACProjectChannels.cpp

clean:
	-rm $(OBJS) alacconvert
	




