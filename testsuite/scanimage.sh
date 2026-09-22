#!/bin/sh -u
# testsuite/scanimage.sh -- scanimage test
# Copyright (C) 2026 Sane Developers.
#
# License: GPL-3.0+
#
# Runs the scanimage -T tests against the test backend.

installed=
case ${1:-} in
    --installed) installed=1;;
esac

SRCFILE="${srcdir:-$(dirname "$0")}/testfile.pnm"
OUTFILE="$PWD/outfile.pnm"

CONFDIR=

if test -n "$installed"; then
    if command -v scanimage > /dev/null 2>&1; then
        SCANIMAGE=scanimage
    else
        SCANIMAGE=../frontend/scanimage
        if test ! -x "$SCANIMAGE"; then
            echo "scanimage test: scanimage not built, skipping"
            exit 0
        fi
    fi
else
    SCANIMAGE=../frontend/scanimage

    if test ! -x "$SCANIMAGE"; then
        echo "scanimage test: scanimage not built, skipping"
        exit 0
    fi

    if test ! -f ../backend/libsane-test.la; then
        echo "scanimage test: test backend not built, skipping"
        exit 0
    fi

    CONFDIR="$(mktemp -d)"
    export SANE_CONFIG_DIR="$CONFDIR"
    LD_LIBRARY_PATH="$(cd ../backend/.libs && pwd)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH

    printf 'test\n' > "$SANE_CONFIG_DIR/dll.conf"
    printf 'resolution 50.0\n' > "$SANE_CONFIG_DIR/test.conf"
fi

cleanup() {
    rm -f "$OUTFILE"
    test -n "$CONFDIR" && rm -rf "$CONFDIR"
}

trap cleanup EXIT INT TERM

echo "---> Trying flatbed scanner"
"$SCANIMAGE" -d test -T || exit 1
echo "<--- Flatbed scanner succeeded"

echo "---> Trying three pass flatbed scanner"
"$SCANIMAGE" -d test --mode Color --three-pass=yes -T || exit 1
echo "<--- Three pass scanner succeeded"

echo "---> Trying hand scanner"
"$SCANIMAGE" -d test --hand-scanner=yes -T || exit 1
echo "<--- Hand scanner succeeded"

echo "---> Checking 16 bit color mode"
"$SCANIMAGE" -d test --mode Color --depth 16 --test-picture "Color pattern" \
    --resolution 50 -y 20 -x 20 > "$OUTFILE" || exit 1
cmp -s "$SRCFILE" "$OUTFILE" || exit 1
echo "<--- 16 bit color mode succeeded"

echo "**** All tests passed"
