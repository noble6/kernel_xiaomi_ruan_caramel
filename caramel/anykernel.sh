### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## caramel for the Redmi Pad Pro 5G / POCO Pad 5G (ruan)

### AnyKernel setup
# global properties
properties() { '
kernel.string=caramel kernel for ruan
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
# boot shell variables
BLOCK=boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# GKI: only the kernel in boot is replaced. The vendor modules, dtb and
# ramdisks of the ROM stay as they are.
split_boot;
flash_boot;
## end boot install
