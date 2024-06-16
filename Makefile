CFLAGS?=-Wall -Wextra -Wframe-larger-than=2048 -O0 -pedantic -std=c89 -ggdb $(shell pkg-config --cflags sdl2 SDL2_ttf)
LIBS=$(shell pkg-config --libs sdl2 SDL2_ttf) -lpng -lgmp

all: main

main: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LIBS)

LIBPNGVER=libpng-1.6.43
LIBZVER=zlib-1.3.1
GMPVER=gmp-6.3.0
SDL2VER=SDL2-2.30.3
SDL2_TTFVER=SDL2_ttf-2.22.0

deps_sources: deps libpng libgmp libSDL2 libSDL2_ttf

deps:
	mkdir deps

libz: deps/$(LIBZVER)
	cd $< && \
	CC=$(CC) ./configure --static && \
	$(MAKE)

libpng: deps/$(LIBPNGVER) libz 
	cd $< && \
	# This is hackish but it doesn't work otherwise ¯\_(ツ)_/¯ \
	CC="$(CC) -L../$(LIBZVER) -I../$(LIBZVER)" ./configure --disable-shared && \
	sed -i 's/\(build_libtool_need_lc=\)yes/\1no/' libtool && \
	$(MAKE)

libgmp: deps/$(GMPVER)
	cd $< && \
	sed -i '10128s/conftest)/conftest.exe)/' configure && \
	CC=$(CC) ./configure --disable-shared --disable-assembly && \
	sed -i 's/\(build_libtool_need_lc=\)yes/\1no/' libtool && \
	sed -i 's|./gen-\w*|&$$(EXEEXT_FOR_BUILD)|' Makefile && \
	$(MAKE)

libSDL2:
	wget -O - https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-mingw.tar.gz | tar -C deps -xz

libSDL2_ttf:
	wget -O - https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-devel-2.22.0-mingw.tar.gz | tar -C deps -xz

deps/$(GMPVER):
	wget -O - https://ftp.gnu.org/gnu/gmp/$(GMPVER).tar.gz | tar -C deps -xz

deps/$(LIBPNGVER):
	wget -O - https://download.sourceforge.net/libpng/$(LIBPNGVER).tar.gz | tar -C deps -xz

deps/$(LIBZVER):
	wget -O - https://www.zlib.net/$(LIBZVER).tar.gz | tar -C deps -xz

mingw_build: deps_sources
	$(CC) -o $@ main.c $(CFLAGS) -Ideps/$(LIBPNGVER) \
			-Ideps/$(LIBZVER) -Ideps/$(GMPVER) \
			-Ideps/$(SDL2VER)/x86_64-w64-mingw32/include/SDL2 \
			-Ideps/$(SDL2_TTFVER)/x86_64-w64-mingw32/include/SDL2 \
			-Ldeps/$(LIBPNGVER)/.libs \
			-Ldeps/$(LIBZVER) -Ldeps/$(GMPVER)/.libs \
			-Ldeps/$(SDL2VER)/x86_64-w64-mingw32/lib \
			-Ldeps/$(SDL2_TTFVER)/x86_64-w64-mingw32/lib \
			-l:libpng16.a -l:libz.a -l:libgmp.a -l:libSDL2.a \
			-l:libSDL2_ttf.a -l:libSDL2main.a -lole32 \
			-lwinmm -lgdi32 -loleaut32 -lcfgmgr32 -limm32 \
			-lversion -lsetupapi -lrpcrt4
