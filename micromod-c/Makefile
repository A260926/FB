
CC=gcc
CFLAGS=-ansi -pedantic -Wall -Og

CORE=micromod.c micromod_core.c micromod_player.c

all: micromod-sdl

clean:
	rm -f mod2wav micromod-sdl micromod-w32

mod2wav: mod2wav.c $(CORE) micromod.h
	$(CC) $(CFLAGS) $(CORE) mod2wav.c -o mod2wav

micromod-sdl: sdlplayer.c $(CORE) micromod.h
	$(CC) $(CFLAGS) $(CORE) sdlplayer.c -o micromod-sdl `sdl-config --cflags --libs`

micromod-w32: w32player.c $(CORE) micromod.h
	$(CC) $(CFLAGS) $(CORE) w32player.c -o micromod-w32 -lwinmm
