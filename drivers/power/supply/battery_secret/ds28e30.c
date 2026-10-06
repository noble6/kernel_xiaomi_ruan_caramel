
#define pr_fmt(fmt)	"[ds28e30] " fmt
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/regmap.h>
#include <linux/random.h>
#include "ds28e30.h"
#include "w1_common.h"

#define ds_dbg  pr_debug
#define ds_info pr_info
#define ds_log  pr_info
#define ds_err  pr_err

unsigned short CRC16;
static wait_queue_head_t ds28e30_wait;
static raw_spinlock_t io_lock;
static int W1BusNum = 0x00;
const short oddparity[16] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };

extern void __iomem *gpio_8_cfg, *gpio_8_inout;
extern void __iomem *gpio_9_cfg, *gpio_9_inout;

// ds28e30 pull low 7~10us during reset
static unsigned char ow_reset(int num)
{
	u64 pre, now;
	unsigned char presence = 0xFF;
	unsigned long flags;

	raw_spin_lock_irqsave(&io_lock, flags);
	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	udelayk(48);
	w1_gpio_set_level(num, 1);
	w1_gpio_set_input(num);
	pre = ktime_get_boottime_ns();
	while (1) {
		now = ktime_get_boottime_ns();
		presence = w1_gpio_read_level(num);
		if (!presence)
			break;
		// 10us timeout
		if (now - pre > 10 * 1000)
			break;
	}
	udelayk(50);
	raw_spin_unlock_irqrestore(&io_lock, flags);

	return presence;
}

static unsigned char read_bit(int num)
{
	unsigned int vamm;

	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	w1_gpio_set_input(num);
	w1_gpio_set_input(num);
	w1_gpio_set_input(num);
	vamm = w1_gpio_read_level(num);
	udelayk(12);

	return vamm;
}

void write_bit(int num, char bitval)
{
	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	if (bitval) {
		w1_gpio_set_level(num, 0);
		w1_gpio_set_level(num, 1);
	}

	udelayk(6);
	w1_gpio_set_level(num, 1);
	udelayk(6);
}

static unsigned char read_byte(int num)
{
	unsigned char i;
	unsigned char value = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&io_lock, flags);
	for (i = 0; i < 8; i++) {
		if (read_bit(num))
			value |= 0x01 << i;	// reads byte in, one byte at a time and then shifts it left
	}
	raw_spin_unlock_irqrestore(&io_lock, flags);

	return value;
}

static void write_byte(int num, char val)
{
	unsigned char i;
	unsigned char temp;
	unsigned long flags;

	raw_spin_lock_irqsave(&io_lock, flags);
	// writes byte, one bit at a time
	for (i = 0; i < 8; i++) {
		temp = val >> i;	// shifts val right ‘i’ spaces
		temp &= 0x01;	// copy that bit to temp
		write_bit(num, temp);	// write bit in temp into
	}
	raw_spin_unlock_irqrestore(&io_lock, flags);
}

unsigned short docrc16(unsigned short data)
{
	data = (data ^ (CRC16 & 0xff)) & 0xff;
	CRC16 >>= 8;
	if (oddparity[data & 0xf] ^ oddparity[data >> 4])
		CRC16 ^= 0xc001;
	data <<= 6;
	CRC16 ^= data;
	data <<= 1;
	CRC16 ^= data;
	return CRC16;
}

int standard_cmd_flow(unsigned char *write_buf, int write_len,
	int delay_ms, int expect_read_len,
	unsigned char *read_buf, int *read_len)
{
	int i;
	int pkt_len;
	int retry_rst_cnt = 0;
	int retry_crc1_cnt = 0;
	int retry_crc2_cnt = 0;
	unsigned char pkt[256] = { 0 };

cmd_retry:
	pkt_len = 0;

	if ((ow_reset(W1BusNum)) != 0) {
		retry_rst_cnt++;
		ds_err("reset device failed, retry cnt: %d\n", retry_rst_cnt);
		wait_event_timeout(ds28e30_wait, 0,
			msecs_to_jiffies(RETRY_MAX_DELAY_MS));
		if (retry_rst_cnt < RETRY_MAX_COUNT)
			goto cmd_retry;
		else
			goto exit_out;
	}

	write_byte(W1BusNum, CMD_SKIP_ROM);
	// Construct write block, start with XPC command
	pkt[pkt_len++] = CMD_START;
	// Add length
	pkt[pkt_len++] = write_len;
	// write (first byte will be sub-command)
	memcpy(&pkt[pkt_len], write_buf, write_len);
	pkt_len += write_len;
	//send packet to DS28E30
	ds_info("w1w: %02x\n", CMD_SKIP_ROM);
	print_hex_dump(KERN_ERR, "[ds28e30] w1w: ",
		DUMP_PREFIX_NONE, 16, 1, pkt, pkt_len, 0);
	for (i = 0; i < pkt_len; i++)
		write_byte(W1BusNum, pkt[i]);
	// read two CRC bytes
	pkt[pkt_len++] = read_byte(W1BusNum);
	pkt[pkt_len++] = read_byte(W1BusNum);
	ds_err("w1r: %02x %02x\n", pkt[pkt_len - 2], pkt[pkt_len - 1]);
	// check CRC16
	CRC16 = 0;
	for (i = 0; i < pkt_len; i++)
		docrc16(pkt[i]);
	if (CRC16 != 0xB001) {
		retry_crc1_cnt++;
		ds_err("first crc error, retry cnt: %d\n", retry_crc1_cnt);
		wait_event_timeout(ds28e30_wait, 0,
			msecs_to_jiffies(RETRY_MAX_DELAY_MS));
		if (retry_crc1_cnt < RETRY_MAX_COUNT)
			goto cmd_retry;
		else
			goto exit_out;
	}

	if (delay_ms > 0) {
		// Send release byte, start strong pull-up
		write_byte(W1BusNum, 0xAA);
		ds_err("w1w: %02x\n", 0xAA);
		// optional delay
		wait_event_timeout(ds28e30_wait, 0, msecs_to_jiffies(delay_ms));
	}

	// read FF and the length byte
	pkt[0] = read_byte(W1BusNum);
	pkt[1] = read_byte(W1BusNum);
	*read_len = pkt[1];
	ds_info("w1r: %02x %02x\n", pkt[0], pkt[1]);
	// read packet
	for (i = 0; i < expect_read_len + 2; i++)
		read_buf[i] = read_byte(W1BusNum);
	print_hex_dump(KERN_ERR, "[ds28e30] w1r: ",
		DUMP_PREFIX_NONE, 16, 1, read_buf, expect_read_len + 2, 0);
	// check CRC16
	CRC16 = 0;
	docrc16(*read_len);
	for (i = 0; i < (expect_read_len + 2); i++)
		docrc16(read_buf[i]);
	if (expect_read_len != *read_len) {
		*read_len = expect_read_len;
		ds_err("receive length error\n");
	}
	if (CRC16 != 0xB001) {
		retry_crc2_cnt++;
		ds_err("second crc error, retry cnt: %d\n", retry_crc2_cnt);
		wait_event_timeout(ds28e30_wait, 0,
			msecs_to_jiffies(RETRY_MAX_DELAY_MS));
		if (retry_crc2_cnt < RETRY_MAX_COUNT)
			goto cmd_retry;
		else
			goto exit_out;
	}

exit_out:
	if (retry_rst_cnt < RETRY_MAX_COUNT &&
		retry_crc1_cnt < RETRY_MAX_COUNT &&
		retry_crc2_cnt < RETRY_MAX_COUNT)
		return DS_TRUE;
	else
		return DS_FALSE;
}

int ds28e30_cmd_writeMemory(int pg, unsigned char *data)
{
	unsigned char write_buf[50];
	int write_len;
	unsigned char read_buf[255];
	int read_len;

	write_len = 0;
	write_buf[write_len++] = CMD_WRITE_MEM;
	write_buf[write_len++] = pg;
	memcpy(&write_buf[write_len], data, 32);
	write_len += 32;
	// preload read_len with expected length
	read_len = 1;
	// default failure mode
	if (standard_cmd_flow(write_buf, write_len, DELAY_DS28E30_EE_WRITE_TWM, read_len,
	     read_buf, &read_len)) {
		// check result
		if (read_len == 1)
			return (read_buf[0] == RESULT_SUCCESS);
	}

	return DS_FALSE;
}

int ds28e30_cmd_readMemory(int pg, unsigned char *data)
{
	unsigned char write_buf[10];
	int write_len;
	unsigned char read_buf[255];
	int read_len;

	write_len = 0;
	write_buf[write_len++] = CMD_READ_MEM;
	write_buf[write_len++] = pg;
	// preload read_len with expected length
	read_len = 33;

	if (standard_cmd_flow(write_buf, write_len, DELAY_DS28E30_EE_READ_TRM, read_len,
	     read_buf, &read_len)) {
		if (read_len == 33) {
			if (read_buf[0] == RESULT_SUCCESS) {
				memcpy(data, &read_buf[1], 32);
				return DS_TRUE;
			}
		}
	}

	return DS_FALSE;
}

int ds28e30_store_cycle_count(int num, u32 cycles)
{
	u8 page_data[32];
	int ret;

	W1BusNum = num;

	memset(page_data, 0xFF, sizeof(page_data));

	page_data[0] = cycles % 256;
	page_data[1] = cycles / 256;

	ret = ds28e30_cmd_writeMemory(1, page_data);

	return ret;
}

int ds28e30_read_cycle_count(int num, u32 *cycles)
{
	u8 page_data[32];
	int ret;

	W1BusNum = num;

	ret = ds28e30_cmd_readMemory(1, page_data);
	*cycles = page_data[0] + page_data[1] * 256;

	return ret;
}

int ds28e30_init(void)
{
	gpio_8_cfg = ioremap(0xF108000, 8);
	gpio_9_cfg = ioremap(0xF109000, 8);
	if (!gpio_8_cfg || !gpio_9_cfg) {
		pr_err( "<%s>: map gpio-8 or gpio-9 cfg register failed", __func__);
		return -EINVAL;
	}
	gpio_8_inout = gpio_8_cfg + 4;
	gpio_9_inout = gpio_9_cfg + 4;

	raw_spin_lock_init(&io_lock);
	init_waitqueue_head(&ds28e30_wait);

	return 0;
}

int ds28e30_deinit(void)
{
	return 0;
}