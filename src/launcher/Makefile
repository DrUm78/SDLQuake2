PRGNAME     = launcher
CC			= /opt/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-gcc
STRIP       = /opt/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-strip

OBJS		= launcher.o
LDFLAGS     = -lSDL -lSDL_ttf
CFLAGS      = -std=gnu99

# Rules to make executable
$(PRGNAME): $(OBJS)  
	$(CC) $(CFLAGS)  -o $(PRGNAME) $^ $(LDFLAGS)
	$(STRIP) $(PRGNAME)

clean:
	rm -f $(PRGNAME) *.o
