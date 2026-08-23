#!/bin/sh

rm -f SDLQuake2_gcw0.opk
make distclean && make -j16
/opt/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-strip releasegcw/*.so releasegcw/sdlquake2
mkdir -p releasegcw/baseq2/
cp -rf default_gcw.cfg pak0.pak players/ releasegcw/baseq2/
mksquashfs default.gcw0.desktop q2.png releasegcw/baseq2/ releasegcw/gamegcw.so releasegcw/ref_softsdl.so releasegcw/sdlquake2 SDLQuake2_gcw0.opk
