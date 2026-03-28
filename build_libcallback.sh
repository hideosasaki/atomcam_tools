#!/bin/sh
export CROSS_COMPILE=/atomtools/build/cross/mips-uclibc/bin/mipsel-ingenic-linux-uclibc-
BUILDDIR=/atomtools/build/buildroot-2016.02/output/local/libcallback

cp /src/libcallback/*.c $BUILDDIR/
cp /src/libcallback/*.h $BUILDDIR/ 2>/dev/null
cp /src/libcallback/Makefile $BUILDDIR/

cd $BUILDDIR
rm -f libcallback.so
make
cp $BUILDDIR/libcallback.so /src/libcallback/libcallback.so
