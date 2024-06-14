CFLAGS?= -Wall -Wextra -O3 -pedantic -std=c89 -fopenmp -ggdb $(shell pkg-config --cflags sdl2)
LIBS= $(shell pkg-config --libs sdl2) -lpng -lgmp

main.exe: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LIBS)
