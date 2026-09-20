#!/bin/bash

# UWP CI fallback: keep version header generation deterministic on Windows runners.
# The upstream version script currently fails silently under the current GitHub
# Windows/MSYS2 runner while generating xemu-version-macro.h.
XEMU_VERSION="0.8.136"
XEMU_VERSION_MAJOR="0"
XEMU_VERSION_MINOR="8"
XEMU_VERSION_PATCH="136"
XEMU_VERSION_COMMIT="0"
XEMU_COMMIT="uwp-local-games"
XEMU_DATE="UWP custom build"

cat <<EOF
#define XEMU_VERSION       "$XEMU_VERSION"
#define XEMU_VERSION_MAJOR $XEMU_VERSION_MAJOR
#define XEMU_VERSION_MINOR $XEMU_VERSION_MINOR
#define XEMU_VERSION_PATCH $XEMU_VERSION_PATCH
#define XEMU_VERSION_COMMIT $XEMU_VERSION_COMMIT
#define XEMU_COMMIT        "$XEMU_COMMIT"
#define XEMU_DATE          "$XEMU_DATE"
EOF

exit 0
