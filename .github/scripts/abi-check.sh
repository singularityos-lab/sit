#!/bin/sh
set -eu

base_rev=$1
work=$2

mkdir "$work"

build() {
    meson setup "$work/$1-build" "$2" --prefix=/usr --libdir=lib --buildtype=debug \
        -Dintrospection=disabled -Ddocumentation=false -Dman-pages=false -Dscreenshots=false \
        -Dbuild-demos=false -Dbuild-testsuite=false -Dbuild-examples=false -Dbuild-tests=false >/dev/null
    meson compile -C "$work/$1-build"
    meson install -C "$work/$1-build" --destdir "$work/$1-stage" >/dev/null
}

soname() {
    readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p'
}

git worktree add --detach "$work/base" "$base_rev" >/dev/null
build base "$work/base"
build head "$PWD"

old=$(readlink -f "$work/base-stage/usr/lib/libgtk-4.so")
new=$(readlink -f "$work/head-stage/usr/lib/libgtk-4.so")
status=0
if [ "$(soname "$old")" != "$(soname "$new")" ]; then
    echo "libgtk-4.so: soname changed from $(soname "$old") to $(soname "$new"), incompatible changes are allowed"
else
    rc=0
    abidiff --no-added-syms --hd1 "$work/base-stage/usr/include/gtk-4.0" --hd2 "$work/head-stage/usr/include/gtk-4.0" "$old" "$new" || rc=$?
    if [ $((rc & 3)) -ne 0 ]; then
        echo "libgtk-4.so: abidiff could not compare the libraries"
        status=1
    elif [ $((rc & 12)) -ne 0 ]; then
        echo "libgtk-4.so: the ABI changed without a soversion bump; only additions are allowed"
        status=1
    else
        echo "libgtk-4.so: ABI compatible"
    fi
fi

git worktree remove --force "$work/base"
exit $status
