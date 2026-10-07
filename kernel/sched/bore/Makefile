# SPDX-License-Identifier: GPL-2.0-only
# BORE, built in. From AxionOS device/axion/common kernel/modules/bore_sched
# (lineage-23.2, d80f522), which keeps its per-task state in the
# sched_entity's ANDROID_KABI_RESERVE(1..4) and works through the
# android_vh_bore_* hooks.
obj-$(CONFIG_SCHED_BORE) += bore_sched.o
bore_sched-y := bore_sched_main.o
