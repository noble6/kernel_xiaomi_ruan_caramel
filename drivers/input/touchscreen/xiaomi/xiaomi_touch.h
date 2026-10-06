#ifndef __XIAOMI__TOUCH_H
#define __XIAOMI__TOUCH_H
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/slab.h>


/*CUR,DEFAULT,MIN,MAX*/
#define VALUE_TYPE_SIZE 6
#define VALUE_GRIP_SIZE 9
#define MAX_BUF_SIZE 256
#ifndef FACTORY_BUILD
#define CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
#endif
enum MODE_CMD {
	SET_CUR_VALUE = 0,
	GET_CUR_VALUE,
	GET_DEF_VALUE,
	GET_MIN_VALUE,
	GET_MAX_VALUE,
	GET_MODE_VALUE,
	RESET_MODE,
	SET_LONG_VALUE,
};

enum  MODE_TYPE {
	Touch_Game_Mode        = 0,
	Touch_Active_MODE      = 1,
	Touch_UP_THRESHOLD     = 2,
	Touch_Tolerance        = 3,
	/*
	Touch_Wgh_Min          = 4,
	Touch_Wgh_Max          = 5,
	Touch_Wgh_Step         = 6,
	*/
	Touch_Edge_Filter      = 7,
	Touch_Panel_Orientation = 8,
	Touch_Report_Rate      = 9,
	Touch_Fod_Enable       = 10,
	Touch_Aod_Enable       = 11,
	Touch_Resist_RF        = 12,
	Touch_Idle_Time        = 13,
	Touch_Doubletap_Mode   = 14,
	Touch_Pen_ENABLE       = 20,
	Touch_Pen_Hooping      = 22,
	Touch_Pen_Shorthand    = 24,
	TOUCH_STYLUS_SLEEP_STATE = 29,
	Touch_Mode_NUM         = 30,
};

struct xiaomi_touch_interface {
	int touch_mode[Touch_Mode_NUM][VALUE_TYPE_SIZE];
	int (*setModeValue)(int Mode, int value);
	int (*setModeLongValue)(int Mode, int value_len, int *value);
	int (*getModeValue)(int Mode, int value_type);
	int (*getModeAll)(int Mode, int *modevalue);
	int (*resetMode)(int Mode);
	int (*palm_sensor_read)(void);
	int (*palm_sensor_write)(int on);
	int long_mode_len;
	int long_mode_value[MAX_BUF_SIZE];
};

struct xiaomi_touch {
	struct miscdevice 	misc_dev;
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
	struct mutex  mutex;
	struct mutex  palm_mutex;
	struct mutex  psensor_mutex;
	wait_queue_head_t 	wait_queue;
#ifdef CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
	struct mutex pen_connect_strategy_mutex;
#endif // CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
};

/* dump last touch events setup */
#define MAX_TOUCH_ID 12
#define PEN_HOVER_ID MAX_TOUCH_ID - 2
#define PEN_INK_ID   MAX_TOUCH_ID - 1	  //the last one is ink id of pen 
#define LAST_TOUCH_EVENTS_MAX 512

enum touch_state {
	EVENT_INIT,
	EVENT_DOWN,
	EVENT_UP,
};
struct touch_event {
	u32 slot;
	enum touch_state state;
	struct timespec64 touch_time;
};
struct last_touch_event {
	int head;
	struct touch_event touch_event_buf[LAST_TOUCH_EVENTS_MAX];
};
/* dump last touch events setup end */

struct xiaomi_touch_pdata{
	struct xiaomi_touch *device;
	struct xiaomi_touch_interface *touch_data;
	int palm_value;
	bool palm_changed;
	const char *name;
/* dump last touch events setup */
	struct proc_dir_entry  *last_touch_events_proc;
	struct last_touch_event *last_touch_events;
/* dump last touch events setup end */
#ifdef CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
	bool pen_active;
#endif // CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
};

struct xiaomi_touch *xiaomi_touch_dev_get(int minor);

extern struct class *get_xiaomi_touch_class(void);

extern struct device *get_xiaomi_touch_dev(void);

extern int update_palm_sensor_value(int value);

extern int xiaomitouch_register_modedata(struct xiaomi_touch_interface *data);

extern int update_palm_sensor_value(int value);

void last_touch_events_collect(int slot, int state);

#ifdef CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY
int update_pen_connect_strategy_value(bool pen_active);
#endif //CONFIG_TOUCHSCREEN_NEW_PEN_CONNECT_STRATEGY

#endif

