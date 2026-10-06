#ifndef __W1_COMMON_H__
#define __W1_COMMON_H__

#include <linux/kernel.h>

#define read_reg(addr)       (*(volatile u32 __force *)addr)
#define write_reg(val, addr) (*(volatile u32 __force *)addr = val)

void udelayk(int us);
void udelayn(int ns);
inline void w1_gpio_set_input(int num);
inline void w1_gpio_set_output(int num);
inline void w1_gpio_set_level(int num, u8 level);
inline u8 w1_gpio_read_level(int num);

#endif