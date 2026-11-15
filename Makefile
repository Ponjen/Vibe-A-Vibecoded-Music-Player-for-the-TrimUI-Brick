# Invoked by docker-build.sh with CROSS_COMPILE, SYSROOT and DEPS set.
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
CFLAGS  = -O2 -Wall -Wno-unused-result -pthread -D_REENTRANT \
          -I$(DEPS)/include -I$(SYSROOT)/usr/include/SDL2 $(EXTRA_CFLAGS)
LDFLAGS = -L$(DEPS)/lib
LIBS    = -lavformat -lavcodec -lswscale -lswresample -lavutil -lz \
          -lSDL2_ttf -lSDL2 -lm -lpthread

OBJS = src/main.c src/engine.c src/library.c src/browse.c src/dl.c

vibe: $(OBJS) src/engine.h src/library.h src/browse.h src/dl.h
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	$(STRIP) $@

clean:
	rm -f vibe
