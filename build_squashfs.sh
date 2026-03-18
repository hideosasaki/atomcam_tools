#!/bin/sh
UNSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/unsquashfs
MKSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/mksquashfs

rm -rf /tmp/squashfs-root
$UNSQUASHFS -d /tmp/squashfs-root /src/rootfs_hack_upstream.squashfs

# libcallback.so (lib32 is used by iCamera in chroot)
cp /src/libcallback/libcallback.so /tmp/squashfs-root/lib/modules/libcallback.so
cp /src/libcallback/libcallback.so /tmp/squashfs-root/lib32/modules/libcallback.so

# scripts
cp /src/overlay_rootfs/scripts/backchannel.sh /tmp/squashfs-root/scripts/backchannel.sh
chmod +x /tmp/squashfs-root/scripts/backchannel.sh
cp /src/overlay_rootfs/scripts/rtspserver.sh /tmp/squashfs-root/scripts/rtspserver.sh

# hack_ini.cgi (CONFIG_VER fix)
cp /src/overlay_rootfs/var/www/cgi-bin/hack_ini.cgi /tmp/squashfs-root/var/www/cgi-bin/hack_ini.cgi

# go2rtc (rebuilt with reduced audio buffer)
cp /atomtools/build/buildroot-2016.02/output/target/usr/bin/go2rtc /tmp/squashfs-root/usr/bin/go2rtc

# Web frontend
rm -f /tmp/squashfs-root/var/www/bundle*
cp -pr /src/web/frontend/* /tmp/squashfs-root/var/www/

$MKSQUASHFS /tmp/squashfs-root /src/rootfs_hack_new.squashfs -noappend -comp gzip
