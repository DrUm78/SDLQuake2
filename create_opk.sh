#!/bin/sh

# Remove OPK
rm -f *.opk

# Build binaries
git submodule update --init --remote
make distclean && make build_release -j16
cp -rf opk/default_gcw.cfg opk/pak0.pak opk/players/ releasegcw/baseq2/
make clean

# Build launcher
cd src/launcher/
make clean && make
cd -

# Build OPK
mksquashfs src/launcher/launcher opk/launcher.gcw0.desktop opk/q2.png opk/*.ttf opk/*.sh releasegcw/*.so releasegcw/sdlquake2 releasegcw/baseq2/ releasegcw/xatrix/ releasegcw/rogue/ releasegcw/zaero/ releasegcw/smd/ releasegcw/ctf/ SDLQuake2_gcw0.opk
