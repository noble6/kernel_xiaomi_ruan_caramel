# Makefile for NoMount
#
# caramel: vendored from https://github.com/maxsteeel/nomount kernel/src
# (v2.1.0, c7f63e3) without its git history, so the commit count the
# upstream Makefile reads from git is fixed here.
ccflags-y += -DNOMOUNT_COMMIT_COUNT=\"500\"

obj-$(CONFIG_NOMOUNT) += nomount.o
ccflags-y += -std=gnu11 -Wno-declaration-after-statement
