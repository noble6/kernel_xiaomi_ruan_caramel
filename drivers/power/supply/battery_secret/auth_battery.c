#include <linux/module.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
#include "ds28e30.h"
#include "slg.h"
#include "w1_common.h"

#define W1_BUS_RESET_COUNT 3

enum {
	MAIN_SUPPLY = 0,
	SECEON_SUPPLY,
	MAX_SUPPLY,
};

static const char *auth_device_name[] = {
	"main_suppiler",
	"second_supplier",
	"unknown",
};

struct auth_data {
	struct device *dev;
	struct platform_device *pdev;
	struct auth_device *auth_dev[MAX_SUPPLY];
	struct power_supply *verify_psy;
	struct power_supply_desc desc;
	bool auth_chip_ok;
	u32 cycle_count;
};

static spinlock_t auth_lock;
static int auth_index = MAX_SUPPLY;
static u8 BatDevId1 = 0xFF, BatDevId2 = 0xFF;

extern void __iomem *gpio_8_cfg, *gpio_8_inout;
extern void __iomem *gpio_9_cfg, *gpio_9_inout;

int lc_auth_get_cycle_count(void)
{
	int cycle_count1 = 0, cycle_count2 = 0;

	if (BatDevId1 == 0x01 && BatDevId2 == 0x01) {
		slg_read_cycle_count(1, &cycle_count1);
		slg_read_cycle_count(2, &cycle_count2);
	} else if (BatDevId1 == 0x02 && BatDevId2 == 0x02) {
		ds28e30_read_cycle_count(1, &cycle_count1);
		ds28e30_read_cycle_count(2, &cycle_count2);
	}

	if (cycle_count1 == cycle_count2)
		return cycle_count1;

	if (cycle_count1 != cycle_count2)
		return cycle_count1 > cycle_count2 ? cycle_count1 : cycle_count2;

	return -EINVAL;
}

static void lc_auth_set_cycle_count(int cycle_count)
{
	if (BatDevId1 == 0x01 && BatDevId2 == 0x01) {
		slg_store_cycle_count(1, cycle_count);
		slg_store_cycle_count(2, cycle_count);
	} else if (BatDevId1 == 0x02 && BatDevId2 == 0x02) {
		ds28e30_store_cycle_count(1, cycle_count);
		ds28e30_store_cycle_count(2, cycle_count);
	}
}

static enum power_supply_property verify_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MANUFACTURER,
	//POWER_SUPPLY_PROP_CYCLE_COUNT,
};

static int verify_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct auth_data *info = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = info->auth_chip_ok;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = auth_device_name[auth_index];
		break;
	//case POWER_SUPPLY_PROP_CYCLE_COUNT:
	//	val->intval = lc_auth_get_cycle_count();
	//	info->cycle_count = val->intval;
	//	break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int verify_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct auth_data *info = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		info->cycle_count = val->intval;
		lc_auth_set_cycle_count(info->cycle_count);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int verify_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		return 1;
	default:
		break;
	}

	return 0;
}

static void auth_reset_chip(int num)
{
	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	msleep(30);
	w1_gpio_set_level(num, 1);
	msleep(20);
}

static u8 auth_w1_detect_device(int num)
{
	u8  devid = 0xFF;
	int retry = W1_BUS_RESET_COUNT;
	unsigned long flags = 0;

	auth_reset_chip(num);
RESET_AGAIN:
	spin_lock_irqsave(&auth_lock, flags);
	w1_gpio_set_output(num);
	w1_gpio_set_level(num, 0);
	udelayk(50);
	w1_gpio_set_input(num);
	udelayk(8);
	if (w1_gpio_read_level(num)) {
		pr_err("w1[%d] reset bus error\r\n", num);
		retry--;
		if (retry > 0) {
			spin_unlock_irqrestore(&auth_lock, flags);
			w1_gpio_set_output(num);
			w1_gpio_set_level(num, 0);
			mdelay(30);
			w1_gpio_set_level(num, 1);
			mdelay(20);
			goto RESET_AGAIN;
		} else {
			spin_unlock_irqrestore(&auth_lock, flags);
		}
	} else {
		udelayk(20);
		if (!w1_gpio_read_level(num))
			devid = 0x01;
		else
			devid = 0x02;
		spin_unlock_irqrestore(&auth_lock, flags);
		udelayk(80);
	}

	return devid;
}

static void auth_get_device(struct auth_data *info)
{
	slg_init();
	ds28e30_init();

	BatDevId1 = auth_w1_detect_device(1);
	BatDevId2 = auth_w1_detect_device(2);

	if (BatDevId1 == 0x01 && BatDevId2 == 0x01) {
		auth_index = MAIN_SUPPLY;
		info->auth_chip_ok = true;
		pr_err("acl16slg detected");
	} else if (BatDevId1 == 0x02 && BatDevId2 == 0x02) {
		auth_index = SECEON_SUPPLY;
		info->auth_chip_ok = true;
		pr_err("ds28e30 detected");
	} else {
		info->auth_chip_ok = false;
	}
}

static int lc_auth_battery_probe(struct platform_device *pdev)
{
	struct auth_data *info = NULL;
	struct power_supply_config cfg = { };

	pr_err("%s start\n", __func__);

	info = devm_kzalloc(&(pdev->dev), sizeof(struct auth_data), GFP_KERNEL);
	if (!info) {
		pr_err("%s alloc mem fail\n", __func__);
		return -ENOMEM;
	}

	spin_lock_init(&auth_lock);

	gpio_8_cfg = ioremap(0xF108000, 8);
	gpio_9_cfg = ioremap(0xF109000, 8);
	if (!gpio_8_cfg || !gpio_9_cfg) {
		pr_err( "<%s>: map gpio-8 or gpio-9 cfg register failed", __func__);
		return -EINVAL;
	}
	gpio_8_inout = gpio_8_cfg + 4;
	gpio_9_inout = gpio_9_cfg + 4;

	auth_get_device(info);

	cfg.drv_data = info;
	info->desc.name = "batt_verify";
	info->desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->desc.properties = verify_props;
	info->desc.num_properties = ARRAY_SIZE(verify_props);
	info->desc.get_property = verify_get_property;
	info->desc.set_property = verify_set_property;
	info->desc.property_is_writeable = verify_is_writeable;
	info->verify_psy = power_supply_register(NULL, &(info->desc), &cfg);
	if (!(info->verify_psy)) {
		pr_err("%s register verify psy fail\n", __func__);
	}

	info->dev = &pdev->dev;
	info->pdev = pdev;
	platform_set_drvdata(pdev, info);

	pr_err("%s end\n", __func__);

	return 0;
}

static int lc_auth_battery_remove(struct platform_device *pdev)
{
	struct auth_data *info = platform_get_drvdata(pdev);

	power_supply_unregister(info->verify_psy);

	ds28e30_deinit();
	slg_deinit();

	return 0;
}

static const struct of_device_id auth_battery_dt_match[] = {
	{.compatible = "lc,auth-battery"},
	{},
};

static struct platform_driver lc_auth_battery_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "lc,auth-battery",
		.of_match_table = auth_battery_dt_match,
	},
	.probe = lc_auth_battery_probe,
	.remove = lc_auth_battery_remove,
};

module_platform_driver(lc_auth_battery_driver);
MODULE_DESCRIPTION("auth battery driver");
MODULE_LICENSE("GPL v2");

