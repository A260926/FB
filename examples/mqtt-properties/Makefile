PROG ?= example                   # Program we are building
DELETE ?= rm -rf                  # Command to remove files
OUT ?= -o $(PROG)                 # Compiler argument for output file
SOURCES = main.c mongoose.c       # Source code files
CFLAGS ?= -W -Wall -Wextra -g -I. # Build options

# Mongoose build options. See https://mongoose.ws/documentation/#build-options
CFLAGS_MONGOOSE += -DMG_ENABLE_LINES=1

ifeq ($(OS),Windows_NT)           # Windows settings for MinGW compiler
  PROG = example.exe
  CC = gcc
  CFLAGS += -lws2_32
  DELETE = cmd /C del /Q /F /S
endif

all: $(PROG)
	$(RUN) $(PROG) $(ARGS)

$(PROG): $(SOURCES)
	$(CC) $(SOURCES) $(CFLAGS) $(CFLAGS_MONGOOSE) $(CFLAGS_EXTRA) $(OUT)

clean:
	$(DELETE) $(PROG) *.o *.obj *.exe *.dSYM
