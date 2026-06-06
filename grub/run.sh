cp ../rtems-6.1/build/x86_64/amd64/testsuites/samples/hello.exe ./
cp hello.exe RTEMS-GRUB/rtems
makefs -t msdos -s 50m RTEMS-GRUB.img RTEMS-GRUB
qemu-system-x86_64 -m 512 -serial stdio -no-reboot -no-shutdown --bios /usr/share/ovmf/OVMF.fd -drive file=RTEMS-GRUB.img,format=raw
