#!/usr/bin/env bash
# Installs The Imaging Source's USB3 Vision GenTL driver on non-Debian distributions (e.g. Arch)
# by unpacking TIS's .deb into the same locations the Debian package uses.
# On Ubuntu/Debian, install the .deb with apt instead: sudo apt install ./ic4-gentl-driver-usb3vision_*.deb
#
# Download the driver ("GenTL Producer / IC4 Driver", Linux amd64 .deb) from
#   https://www.theimagingsource.com/en-us/support/download/
# then run:
#   sudo scripts/install_tis_driver.sh path/to/ic4-gentl-driver-usb3vision_<version>_amd64.deb
set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "Run with sudo: sudo $0 <driver .deb>" >&2
    exit 1
fi
DEB="${1:?Usage: sudo $0 path/to/ic4-gentl-driver-usb3vision_<version>_amd64.deb}"
[ -f "$DEB" ] || { echo "Not found: $DEB" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf -- "${TMP:?}"' EXIT

echo "Unpacking $DEB"
(cd "$TMP" && bsdtar -xf "$(realpath "$DEB")" && mkdir data && bsdtar -xf data.tar.* -C data)
D="$TMP/data"

install -Dm755 "$D/usr/bin/ic4-gentl-u3v-service"                         /usr/bin/ic4-gentl-u3v-service
install -Dm755 "$D/usr/lib/x86_64-linux-gnu/gentl/tis/libic4-gentl-u3v.cti" /usr/lib/x86_64-linux-gnu/gentl/tis/libic4-gentl-u3v.cti
install -Dm644 "$D/usr/lib/systemd/system/ic4-gentl-u3v.service"          /etc/systemd/system/ic4-gentl-u3v.service
install -Dm644 "$D/usr/lib/udev/rules.d/80-ic4-gentl-u3v.rules"           /etc/udev/rules.d/80-ic4-gentl-u3v.rules
install -Dm644 "$D"/etc/profile.d/ic4-gentl-driver-usb3vision-gentl-path.sh /etc/profile.d/ic4-gentl-driver-usb3vision-gentl-path.sh
mkdir -p /usr/share/theimagingsource
cp -r "$D/usr/share/theimagingsource/ic4-gentl-driver-usb3vision" /usr/share/theimagingsource/

udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=199e
systemctl daemon-reload
systemctl enable --now ic4-gentl-u3v.service

echo
systemctl --no-pager --lines=5 status ic4-gentl-u3v.service || true
echo
echo "Done. Unplug and replug the camera."
