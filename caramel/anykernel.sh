### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## caramel for the Redmi Pad Pro 5G / POCO Pad 5G (ruan)

### AnyKernel setup
# global properties
properties() { '
kernel.string=caramel v4 for ruan by 4-8-2-1-1-7
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=ruan
device.name2=
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties


### AnyKernel install
# The active slot. AK3 looks only at getprop and /proc/cmdline, but ruan
# boots with header v4: the bootloader passes androidboot.slot_suffix in
# /proc/bootconfig, and getprop in the recovery updater only reads
# build.prop files. So find it here and give AK3 the slot's block device.
slot=$(getprop ro.boot.slot_suffix 2>/dev/null);
[ "$slot" ] || slot=$(sed -n 's/^androidboot\.slot_suffix = "\(.*\)"$/\1/p' /proc/bootconfig 2>/dev/null);
[ "$slot" ] || slot=$(grep -o 'androidboot.slot_suffix=[^ ]*' /proc/cmdline | cut -d= -f2);
case $slot in
  _a|_b) ;;
  *) abort "Unable to determine the active slot. Aborting...";;
esac;
for byname in /dev/block/bootdevice/by-name /dev/block/by-name; do
  [ -e $byname/boot$slot ] && break;
done;
[ -e $byname/boot$slot ] || abort "boot$slot not found. Aborting...";
ui_print "Installing to boot$slot";

# boot shell variables
BLOCK=$byname/boot$slot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# GKI: only the kernel in boot is replaced. The vendor modules, dtb and
# ramdisks of the ROM stay as they are.
split_boot;
flash_boot;
## end boot install
