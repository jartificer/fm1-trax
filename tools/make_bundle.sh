#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The install bundle: the firmware package, the installer, the rescue tool, INSTALL.md and the licence
# in one zip, build/fm1trax-VERSION.zip (VERSION: the commit, or the first argument).
#   tools/make_bundle.sh [VERSION] [PACKAGE]      (PACKAGE: build/fm1trax.fwsc, else web/fm1trax.fwsc)
set -e
cd "$(dirname "$0")/.."
V="${1:-$(git rev-parse --short HEAD)}"
P="${2:-}"
[ -n "$P" ] || { [ -f build/fm1trax.fwsc ] && P=build/fm1trax.fwsc || P=web/fm1trax.fwsc; }
D="build/bundle/fm1trax-$V"
rm -rf "$D" "build/fm1trax-$V.zip"
mkdir -p "$D"
cp "$P" "$D/fm1trax.fwsc"
cp tools/fm1_install.py tools/fm1_rescue.py INSTALL.md LICENSE "$D/"
(cd build/bundle && python3 -c "import shutil, sys; shutil.make_archive('../fm1trax-' + sys.argv[1], 'zip', '.', 'fm1trax-' + sys.argv[1])" "$V")
echo "bundle: build/fm1trax-$V.zip ($P)"
