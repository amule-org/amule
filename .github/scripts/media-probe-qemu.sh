#!/usr/bin/env bash
# CI-only disposable initramfs. No host service, disk or network is exposed to it.
set -euo pipefail
root="$PWD/media-probe-vm/root"
mkdir media-probe-vm
{
  git rev-parse HEAD
  "${CROSS}-g++" --version
  "qemu-system-${EMULATOR}" --version
} > media-probe-vm/provenance.txt
cmake -S src/mediaprobe -B build-probe -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_CXX_COMPILER="${CROSS}-g++" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build-probe --parallel 2
sha256sum build-probe/src/amule-media-probe build-probe/media-probe-sandbox-test \
  >> media-probe-vm/provenance.txt

# debootstrap checks Debian's signed archive metadata. The guest has its own
# Python and libc; only the statically linked candidate binaries are copied in.
sudo debootstrap --foreign --arch="$ARCH" --include=python3,busybox,ca-certificates \
  --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
  trixie "$root" https://deb.debian.org/debian > media-probe-vm/bootstrap.log 2>&1
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive /debootstrap/debootstrap --second-stage >> media-probe-vm/bootstrap.log 2>&1
if [[ "$KERNEL_ARCH" != "$ARCH" ]]; then
  # armel userspace also runs on the ARMv7 kernel used for armhf.
  sudo chroot "$root" dpkg --add-architecture "$KERNEL_ARCH"
fi
sudo chroot "$root" apt-get update >> media-probe-vm/bootstrap.log 2>&1
package=$(sudo chroot "$root" apt-cache depends "linux-image-$KERNEL:$KERNEL_ARCH" |
  awk '/Depends: linux-image-[0-9]/{print $2; exit}')
if [[ -z "$package" ]]; then echo 'Cannot resolve the signed Debian kernel package' >&2; exit 1; fi
package="${package%:*}:$KERNEL_ARCH"
(cd "$root"; sudo chroot . apt-get download "$package") >> media-probe-vm/bootstrap.log 2>&1
sudo dpkg-deb -x "$root"/linux-image-*.deb media-probe-vm/kernel
find media-probe-vm/kernel/boot -type f -name 'vmlinuz-*' -o -name 'vmlinux-*' |
  sort > media-probe-vm/kernel-path.txt
read -r kernel < media-probe-vm/kernel-path.txt
test -n "$kernel"
sha256sum "$kernel" >> media-probe-vm/provenance.txt

sudo mkdir -p "$root/tests" "$root/proc" "$root/sys" "$root/dev"
sudo cp src/mediaprobe/test_*.py src/mediaprobe/corpus.py \
  build-probe/src/amule-media-probe build-probe/media-probe-sandbox-test "$root/tests/"
sudo tee "$root/init" > /dev/null <<'GUEST'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
rc=0
/bin/busybox ip link set dev lo up || rc=1
cat /etc/os-release
uname -a
python3 --version
cd /tests || exit 1
if [ "$rc" = 0 ]; then
  python3 test_helper.py /tests/amule-media-probe HelperTests.test_extracts_wav || rc=1
fi
if [ "$rc" = 0 ]; then
  python3 test_sandbox.py /tests/media-probe-sandbox-test /tests/amule-media-probe || rc=1
  python3 test_corpus.py || rc=1
  python3 test_helper.py /tests/amule-media-probe || rc=1
  python3 test_hostile.py /tests/amule-media-probe || rc=1
fi
echo "MEDIA_PROBE_QEMU_RESULT=$rc"
sync
/bin/busybox poweroff -f
GUEST
sudo chmod 755 "$root/init"
sudo rm -f "$root"/linux-image-*.deb
(cd "$root"; sudo find . -print0 | sudo cpio --null -o --format=newc 2>/dev/null) |
  gzip -1 > media-probe-vm/root.cpio.gz
# No user-mode networking, host shares or forwarded services. A serial result
# marker is required as well as a successful VM exit: a panic is not a pass.
display=()
if [[ "$EMULATOR" = ppc64 ]]; then display=(-vga none); fi
timeout --kill-after=10 1200 "qemu-system-${EMULATOR}" \
  -machine "$MACHINE" -cpu "$CPU" -m 2048 -smp 2 -nographic -no-reboot \
  "${display[@]}" \
  -nic none -kernel "$kernel" -initrd media-probe-vm/root.cpio.gz \
  -append "console=$CONSOLE rdinit=/init panic=-1" 2>&1 | tee media-probe-vm/console.log
grep -qx 'MEDIA_PROBE_QEMU_RESULT=0' <(tr -d '\r' < media-probe-vm/console.log)
