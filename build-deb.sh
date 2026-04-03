#!/bin/bash
set -e

VERSION="1.0.0"
ARCH="${1:-amd64}"
PKG="vfd-daemon"
BUILDDIR="${PKG}_${VERSION}_${ARCH}"

# Build the binary
if [ "$ARCH" = "arm64" ]; then
    make arm64
    BIN="vfd-daemon-arm64"
else
    make x86
    BIN="vfd-daemon-x86"
fi

# Clean and create package tree
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR/usr/bin"
mkdir -p "$BUILDDIR/lib/systemd/system"
mkdir -p "$BUILDDIR/lib/udev/rules.d"
mkdir -p "$BUILDDIR/DEBIAN"

cp "$BIN" "$BUILDDIR/usr/bin/vfd-daemon"
chmod 0755 "$BUILDDIR/usr/bin/vfd-daemon"
cp dist/vfd-daemon.service "$BUILDDIR/lib/systemd/system/"
cp dist/99-ch347.rules "$BUILDDIR/lib/udev/rules.d/"

cat > "$BUILDDIR/DEBIAN/control" <<EOF
Package: ${PKG}
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: 134arg <xen134@outlook.com>
Description: VFD display daemon for CH347/spidev SPI backends
 Drives an 8-digit VFD screen over SPI using a CH347T USB bridge
 or native spidev. Receives system metrics from a remote agent
 over TCP and renders them as bar graphs.
EOF

cat > "$BUILDDIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
udevadm control --reload-rules || true
systemctl daemon-reload || true
EOF
chmod 0755 "$BUILDDIR/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$BUILDDIR"
rm -rf "$BUILDDIR"

echo "Built: ${BUILDDIR}.deb"
