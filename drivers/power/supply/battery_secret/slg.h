#ifndef __SLG_H__
#define __SLG_H__

int slg_store_cycle_count(int busnum, u32 cycles);
int slg_read_cycle_count(int num, u32 *cycles);
int slg_init(void);
int slg_deinit(void);

#endif