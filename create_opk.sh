#!/bin/sh

rm -f SDLQuake2_gcw0.opk
make distclean && make build_release -j16
/opt/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-strip releasegcw/*.so releasegcw/sdlquake2 releasegcw/rogue/*.so releasegcw/xatrix/*.so
cp -rf default_gcw.cfg pak0.pak players/ releasegcw/baseq2/
make clean
mksquashfs *.gcw0.desktop q2.png releasegcw/baseq2/ releasegcw/rogue/ releasegcw/xatrix/ releasegcw/*.so releasegcw/sdlquake2 SDLQuake2_gcw0.opk
