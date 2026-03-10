Daily work

git add <files>
git commit -m "message"
git push


Building

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- foxiot_wolf_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j$(nproc)

Kernel image: arch/arm/boot/zImage


Upstream kernel sync

git fetch linux
git rebase linux/linux-6.12.y
git push --force-with-lease

