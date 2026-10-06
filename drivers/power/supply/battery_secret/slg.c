/*
 *  w1_slg.c - Aisinochip SLG driver
 *
 * Copyright (c) Shanghai Aisinochip Electronics Techology Co., Ltd.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/crc16.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/of_gpio.h>
#include <linux/random.h>
#include <linux/gpio.h>
#include <crypto/akcipher.h>
#include <crypto/rng.h>
#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include "w1_common.h"

#define CRC16_INIT                  0
#define SLG_MEMORY_READ             0xCA
#define RESPONSE_SUCCESS            0xAA
#define W1_BUS_ERROR                -201
#define PARAM_ERROR                 -203
#define RECV_CRC_ERROR              -204
#define RECV_LENGTH_ERROR           -205
#define GENERAL_WAIT_MAX            10
#define FAMILY_CODE                 0xac
#define MI_CID_LSB                  0xF0
#define MI_CID_MSB                  0x04
#define W1_BUS_RESET_COUNT          3
#define W1_BUS_RETRY_COUNT          20
#define W1_SKIP_ROM                 0xCC

#define slg_info pr_info
#define slg_dbg pr_debug
#define slg_err pr_err
#define slg_log pr_debug
#define DEBUG_BYTES printf_hex

#define read_reg(addr)       (*(volatile u32 __force *)addr)
#define write_reg(val, addr) (*(volatile u32 __force *)addr = val)

static spinlock_t w1_lock;
static wait_queue_head_t slg_wait;
static int cmd_need_wait;
static int W1BusNum = 0x00;

extern void __iomem *gpio_8_cfg, *gpio_8_inout;
extern void __iomem *gpio_9_cfg, *gpio_9_inout;

/*write/read ops*/
static void w1_write_bit(int num, u8 bit)
{
	w1_gpio_set_output(num);

	if (bit) {
		w1_gpio_set_level(num, 0);
		udelayk(1);
		w1_gpio_set_level(num, 1);
		udelayk(15);
	} else {
		w1_gpio_set_level(num, 0);
		udelayk(10);
		w1_gpio_set_level(num, 1);
		udelayk(7);
	}
}

static u8 w1_read_bit(int num)
{
	int result;

	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	udelayn(800);
	w1_gpio_set_input(num);
	udelayn(1000);
	result = w1_gpio_read_level(num);
	udelayk(16);

	return result;
}

static void w1_write_8(int num, u8 byte)
{
	int i;
	unsigned long flags = 0;

	spin_lock_irqsave(&w1_lock, flags);
	for (i = 0; i < 8; ++i)
		w1_write_bit(num, (byte >> i) & 0x1);
	spin_unlock_irqrestore(&w1_lock, flags);
}

static void w1_write_block(int num, const u8 * buf, int len)
{
	int i;
	for (i = 0; i < len; ++i)
		w1_write_8(num, buf[i]);	/* calls w1_pre_write */
}

static u8 w1_read_8(int num)
{
	int i;
	int res = 0;
	unsigned long flags = 0;

	spin_lock_irqsave(&w1_lock, flags);
	for (i = 0; i < 8; ++i)
		res |= (w1_read_bit(num) << i);
	spin_unlock_irqrestore(&w1_lock, flags);

	return res;
}

static u8 w1_read_block(int num, u8 * buf, int len)
{
	int i;
	int ret;
	for (i = 0; i < len; ++i)
		buf[i] = w1_read_8(num);
	ret = len;
	return ret;
}

static void wait_slave_release(int num)
{
	int counter;
	int result;

	for (counter = 0; counter < 200; counter++) {
		result = w1_gpio_read_level(num);
		if (result == 1) {
			break;
		}
		udelay(1);
	}
}

static int w1_reset_bus(int num)
{
	int result = 1;
	int counter = 0;
	int retry = W1_BUS_RESET_COUNT;
	int poweroff = 0;
	unsigned long flags = 0;

	spin_lock_irqsave(&w1_lock, flags);
RESET_AGAIN:
	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	udelay(70);
	w1_gpio_set_level(num, 1);
	w1_gpio_set_input(num);
	for (counter = 0; counter < 100; counter++) {
		result = w1_gpio_read_level(num);
		if (result == 0) {
			break;
		}
	}
	udelay(1);
	if (result == 0) {
		//there is a slave, wait the bus release
		w1_gpio_set_input(num);
		wait_slave_release(num);
	} else {
		retry--;
		if (retry > 0) {
			msleep(2);
			goto RESET_AGAIN;
		} else {
			if (poweroff == 0) {
				printk
				    ("[slg] slg reset failed, poweroff slg and retry again\n");
				poweroff = 1;
				w1_gpio_set_output(num);
				w1_gpio_set_level(num, 0);
				// delay 30ms let slg poweroff
				msleep(30);
				w1_gpio_set_level(num, 1);
				// delay 10ms let slg poweron
				msleep(10);
				retry = W1_BUS_RESET_COUNT;
				goto RESET_AGAIN;
			}
		}
	}
	spin_unlock_irqrestore(&w1_lock, flags);
	return result;
}

static int w1_reset_select_slave(int num)
{
	if (w1_reset_bus(num))
		return -1;

	w1_write_8(num, W1_SKIP_ROM);

	return 0;
}

void *hex2asc(unsigned char *dest, unsigned char *src, unsigned int len)
{
	unsigned int i;
	unsigned char *p;

	p = dest;
	if (len % 2)
		*dest++ = (*src++ & 0x0F) + 0x30;
	for (i = 0; i < (len / 2); i++) {
		*dest++ = ((*src & 0xF0) >> 4) + 0x30;
		*dest++ = (*src++ & 0x0F) + 0x30;
	}
	while (p != dest) {
		if (*p >= 0x3A)
			*p += 7;
		p++;
	}
	return ((unsigned char *) dest);
}

void printf_hex(unsigned char *output, int output_len)
{
	char buffer[1024];

	if (output_len == 0) {
		return;
	} else if (output_len > (sizeof(buffer) / 2)) {
		output_len = (sizeof(buffer) / 2);
	}
	memset(buffer, 0x00, sizeof(buffer));
	hex2asc(buffer, output, output_len << 1);
	slg_err("w1 data: %s\n", buffer);
}

/*
 * Send and Receive data by W1 bus
 *
 * @param input: Send data buffer
 * @param input_len: Send data length
 * @param output: Received data buffer
 * @param output_len: Received data length
 * @return: 0=Success; others=failure, see Error code
 *
 */
static int bus_send_recv(unsigned char *input, int input_len,
			 unsigned char *output, int *output_len)
{
	unsigned char buffer[512];
	unsigned char recv[512];
	unsigned short calc_crc;
	unsigned short recv_crc;
	int index, len;
	int ret;
	unsigned short retry = W1_BUS_RETRY_COUNT;

	if (input_len > 500) {
		return PARAM_ERROR;
	}

	memset(buffer, 0, sizeof(buffer));
	memset(recv, 0, sizeof(recv));
	memcpy(buffer, input, input_len);
	index = input_len;
	calc_crc = crc16(CRC16_INIT, buffer, index);
	calc_crc ^= 0xFFFF;
	buffer[index++] = calc_crc >> 8;
	buffer[index++] = calc_crc & 0xFF;

RETRY:
	if (w1_reset_select_slave(W1BusNum)) {
		slg_err("slg[%d] w1_reset_select_slave failed\n", W1BusNum);
		ret = W1_BUS_ERROR;
		goto END;
	}

	w1_write_block(W1BusNum, buffer, index);
	mdelay(cmd_need_wait);
	memset(recv, 0, sizeof(recv));

	w1_read_block(W1BusNum, recv, 1);
	len = recv[0];
	if (len <= 200) {
		w1_read_block(W1BusNum, &recv[1], len + 2);
	} else {
		len = 0;
	}
	slg_err("w1 send data:\n");
	DEBUG_BYTES(buffer, index);
	slg_err("w1 read length data: %02X\n", recv[0]);
	if (len != 0) {
		slg_err("w1 recv data:\n");
		DEBUG_BYTES(recv, len + 3);
		calc_crc = crc16(CRC16_INIT, recv, len + 1);
		calc_crc ^= 0xFFFF;
		recv_crc = (recv[len + 1] << 8) + recv[len + 2];
		slg_err("w1 recv crc %X, clac crc: %X\n", recv_crc,
		      calc_crc);
		if (recv_crc == calc_crc) {
			slg_err("w1 recv success\n");
			ret = 0;
			memcpy(output, &recv[1], len);
			*output_len = len;
			if (recv[1] == 0x22) {
				ret = RECV_CRC_ERROR;
			}
		} else {
			ret = RECV_CRC_ERROR;
		}
	} else {
		ret = RECV_LENGTH_ERROR;
	}
END:
	if (((ret == RECV_CRC_ERROR) || (ret == RECV_LENGTH_ERROR))
	    && (retry > 0)) {
		printk("[slg] command send failed retry %d\n",
		       W1_BUS_RETRY_COUNT - retry + 1);
		wait_event_timeout(slg_wait, 0, msecs_to_jiffies(100));
		retry--;
		goto RETRY;
	}

	return ret;
}

int slg_memory_read(int index, unsigned char *output, int *output_len)
{
	unsigned char send[8];
	unsigned char recv[64];
	int ret;
	int recv_len;

	cmd_need_wait = GENERAL_WAIT_MAX;
	*output_len = 0;
	slg_info("index = %d\n", index);
	send[0] = SLG_MEMORY_READ;
	send[1] = 1;
	send[2] = index;
	memset(recv, 0xFF, sizeof(recv));
	ret = bus_send_recv(send, 3, recv, &recv_len);
	if (ret == 0) {
		if (recv[0] != RESPONSE_SUCCESS) {
			ret = -recv[0];
		} else {
			memcpy(output, &recv[1], recv_len - 1);
			*output_len = recv_len - 1;
		}
	}

	return ret;
}



/**
 * @brief slg write memory
 *
 * @param index, page index, value range [0~15]
 * @param page, page data
 * @param pageSize, must be less than 32 bytes, when less than 32 bytes
 *        will be filled with 0xFF to 32 bytes in the function
 *
 * @return 0 success, other failed
 */
int slg_memory_write(u8 index, u8 *page, u32 pageSize)
{
    u8 send[64];
    u8 recv[8];
    int ret;
    int recv_len;

    cmd_need_wait = 8;  /* global variable */

    memset(send, 0xFF, sizeof(send));
    memset(recv, 0xFF, sizeof(recv));

    send[0] = 0xDA;
    send[1] = 0x21;
    send[2] = index;
    memcpy(&send[3], page, pageSize);

    ret = bus_send_recv(send, 0x23, recv, &recv_len);

    if(ret == 0)
    {
        if(recv[0] != RESPONSE_SUCCESS)
        {
            ret = -recv[0];
        }
    }

    return ret;
}

int slg_store_cycle_count(int busnum, u32 cycles)
{
	u8 page_data[32];
	int ret;

	W1BusNum = busnum;

	memset(page_data, 0xFF, sizeof(page_data));

	page_data[0] = cycles % 256;
	page_data[1] = cycles / 256;

	ret = slg_memory_write(1, page_data, 32);

	return ret;
}

int slg_read_cycle_count(int num, u32 *cycles)
{
	u8 page_data[32];
	int ret;
	int len = 0;

	W1BusNum = num;

	ret = slg_memory_read(1, page_data, &len);
	*cycles = page_data[0] + page_data[1] * 256;

	return ret;
}

int slg_init(void)
{
	gpio_8_cfg = ioremap(0xF108000, 8);
	gpio_9_cfg = ioremap(0xF109000, 8);
	if (!gpio_8_cfg || !gpio_9_cfg) {
		pr_err( "<%s>: map gpio-8 or gpio-9 cfg register failed", __func__);
		return -EINVAL;
	}
	gpio_8_inout = gpio_8_cfg + 4;
	gpio_9_inout = gpio_9_cfg + 4;

	init_waitqueue_head(&slg_wait);
	spin_lock_init(&w1_lock);

	return 0;
}

int slg_deinit(void)
{

	return 0;
}
