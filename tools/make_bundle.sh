#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The install bundle: the firmware package, the installer, the rescue tool, INSTALL.md and the licence
# in one zip, build/fm1drive-VERSION.zip (VERSION: the commit, or the first argument).
#   tools/make_bundle.sh [VERSION] [PACKAGE]      (PACKAGE: build/fm1drive.fwsc, else web/fm1drive.fwsc)
set -e
cd "$(dirname "$0")/.."
V="${1:-$(git rev-parse --short HEAD)}"
P="${2:-}"
[ -n "$P" ] || { [ -f build/fm1drive.fwsc ] && P=build/fm1drive.fwsc || P=web/fm1drive.fwsc; }
D="build/bundle/fm1drive-$V"
rm -rf "$D" "build/fm1drive-$V.zip"
mkdir -p "$D"
cp "$P" "$D/fm1drive.fwsc"
cp tools/fm1_install.py tools/fm1_rescue.py INSTALL.md LICENSE "$D/"
(cd build/bundle && python3 -c "import shutil, sys; shutil.make_archive('../fm1drive-' + sys.argv[1], 'zip', '.', 'fm1drive-' + sys.argv[1])" "$V")
echo "bundle: build/fm1drive-$V.zip ($P)"
