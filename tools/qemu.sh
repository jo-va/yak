#!/bin/bash

sudo qemu-system-x86_64 \
    -hda disk.img \
    -m 2M \
    -smp 8 \
    -vga std \
    -monitor stdio \
    -no-reboot \
    -enable-kvm \
    -cpu host \
