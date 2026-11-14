# Headless transform-feedback example. No SDL/window/GPU required.
#
#   make -f feedback_replay.make
#   ./feedback_replay

CC ?= cc
TARGET = feedback_replay

all: $(TARGET)

$(TARGET): feedback_replay.c ../../portablegl.h
	$(CC) -std=c99 -O1 -Wall -Wextra -I../.. -o $@ feedback_replay.c -lm

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
