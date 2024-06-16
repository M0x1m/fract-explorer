CFLAGS?= -Wall -Wextra -Wframe-larger-than=2048 -O0 -pedantic -std=c89 -ggdb $(shell pkg-config --cflags sdl2 SDL2_ttf)
LIBS= $(shell pkg-config --libs sdl2 SDL2_ttf) -lpng -lgmp

main.exe: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LIBS)
