
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include "w1_common.h"


void __iomem *gpio_8_cfg, *gpio_8_inout;
void __iomem *gpio_9_cfg, *gpio_9_inout;

void udelayk(int us)
{
	u64 pre, last;

	pre = ktime_get_boottime_ns();
	while (1) {
		last = ktime_get_boottime_ns();
		if (last - pre >= us * 1000)
			break;
	}
}

void udelayn(int ns)
{
	u64 pre, last;

	pre = ktime_get_boottime_ns();
	while (1) {
		last = ktime_get_boottime_ns();
		if (last - pre >= ns)
			break;
	}
}

inline void w1_gpio_set_input(int num)
{
	if (num == 0x01)
		write_reg((0 << 9) | (7 << 6) | (0 << 0), gpio_8_cfg);
	else
		write_reg((0 << 9) | (7 << 6) | (0 << 0), gpio_9_cfg);
}

inline void w1_gpio_set_output(int num)
{
	if (num == 0x01)
		write_reg((1 << 9) | (7 << 6) | (0 << 0), gpio_8_cfg);
	else
		write_reg((1 << 9) | (7 << 6) | (0 << 0), gpio_9_cfg);
}

inline void w1_gpio_set_level(int num, u8 level)
{
	if (num == 0x01) {
		write_reg(level << 1, gpio_8_inout);
		wmb();
	} else {
		write_reg(level << 1, gpio_9_inout);
		wmb();
	}
}

inline u8 w1_gpio_read_level(int num)
{
	if (num == 0x01)
		return read_reg(gpio_8_inout) & 0x01;
	else
		return read_reg(gpio_9_inout) & 0x01;
}
