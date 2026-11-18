#!/bin/sh
# The PNGs and the XPM, from the one SVG, with rsvg-convert and ImageMagick.
# What this makes is kept in the repository, so neither is needed to build.
set -e
cd "$(dirname "$0")"

for s in 16 22 24 32 48 64 128 256; do
    rsvg-convert -w $s -h $s imperaterm.svg -o imperaterm-$s.png
done

# The XPM is the 48 with each pixel either there or not.
convert imperaterm-48.png -channel A -threshold 50% +channel xpm:- |
    sed 's/^static .*\[\] = {$/static char *imperaterm_xpm[] = {/' > imperaterm.xpm
