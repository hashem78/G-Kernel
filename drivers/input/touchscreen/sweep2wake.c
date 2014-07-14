/*
 * drivers/input/touchscreen/sweep2wake.c
 *
 * sweep2wake, sweep2sleep, doubletap2wake
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/sweep2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>
#ifndef CONFIG_HAS_EARLYSUSPEND
#error "Need earlysuspend"
#else
#include <linux/earlysuspend.h>
#endif
#include <asm-generic/cputime.h>

/* uncomment since no touchscreen defines android touch, do that here */
//#define ANDROID_TOUCH_DECLARED

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>, rob43"
#define DRIVER_DESCRIPTION "Sweep2wake, sweep2sleep, doubletap2wake for GS4 Mini"
#define DRIVER_VERSION "1.0"
#define LOGTAG "[power actions]: "
#define SWEEP2WAKE_LABEL "sweep2wake"
#define SWEEP2SLEEP_LABEL "sweep2sleep"
#define DOUBLETAP2WAKE_LABEL "doubletap2wake"
#define NONE_ENABLED_LABEL "no actions enabled"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define S2W_DEBUG				1
#define S2W_DEFAULT				( SWEEP2WAKE_FLAG_SWEEP_SLEEP | SWEEP2WAKE_FLAG_DOUBLE_TAP )
#define S2W_PWRKEY_DUR_MS		100

#define S2W_SCREEN_ON_Y_MIN		800
#define S2W_SCREEN_OFF_Y_MIN	600

#define S2W_X_B1				65
#define S2W_X_B2				475
#define S2W_X_MAX 				540

#define DT2W_TIME           	800
#define DT2W_TOUCHSLOP			150

// uncomment this if X_MAX is not screen width
// #define ENFORCE_X_MAX

#define STATE_TOUCH_1 0
#define STATE_TOUCH_2 1
#define STATE_TOUCH_3 2

/* Resources */
int s2w_switch = S2W_DEFAULT;
static int touch_state = STATE_TOUCH_1;
static int touch_x = 0, touch_y = 0;
static bool touch_x_called = false;
static bool scr_suspended = false;
static bool ignore_touch = false;
static struct input_dev * sweep2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static struct workqueue_struct *s2w_input_wq;
static struct work_struct s2w_input_work;
/* doubletap2wake */
static int tap_count = 0;
static int tap_down_x, tap_down_y;
static cputime64_t tap_time;
static bool power_triggered = false;

/* Read cmdline for s2w */
static int __init read_s2w_cmdline(char *s2w)
{
	/*if (strcmp(s2w, "1") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake enabled. | s2w='%s'\n", s2w);
		s2w_switch = 1;
	} elseif (strcmp(s2w, "2") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'\n", s2w);
		s2w_switch = 2;
	} else if (strcmp(s2w, "0") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'\n", s2w);
		s2w_switch = 0;
	} else {
		pr_info("[cmdline_s2w]: No valid input found. Going with default: | s2w='%u'\n", s2w_switch);
	}*/
	return 1;
}
__setup("s2w=", read_s2w_cmdline);

/* PowerKey work func */
static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	//if (!mutex_trylock(&pwrkeyworklock)) {
		//power_triggered = false;
		input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
		input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
		msleep(S2W_PWRKEY_DUR_MS);
		input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
		input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
		msleep(S2W_PWRKEY_DUR_MS);
	//    mutex_unlock(&pwrkeyworklock);
	//}
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* PowerKey trigger */
static void sweep2wake_pwrtrigger(void) {
	power_triggered = true;
	pr_info(LOGTAG "Triggering power\n");
	schedule_work(&sweep2wake_presspwr_work);
    return;
}

/* reset on finger release */
static void sweep2wake_reset(void) {
	ignore_touch = false;
	touch_state = STATE_TOUCH_1;
}

static void doubletap2wake_reset(void) {
	tap_time = 0;
	tap_down_x = -1;
	tap_down_y = -1;
	tap_count = 0;
}

static void s2w_handle_sweep2sleep(int, int);
static void s2w_handle_sweep2wake(int, int);
static void s2w_handle_doubletap2wake(int, int);
static void s2w_input_callback(struct work_struct *unused) {
	int x = touch_x, y = touch_y;
#if S2W_DEBUG
        pr_info(LOGTAG "x,y(%4d,%4d)\n", x, y);
#endif

	if (scr_suspended) {
		if (touch_y < S2W_SCREEN_OFF_Y_MIN) {
			ignore_touch = true;
		} else if (s2w_switch & SWEEP2WAKE_FLAG_SWEEP_WAKE) {
			s2w_handle_sweep2wake(x, y);
		} else {
			s2w_handle_doubletap2wake(x, y);
		}
	} else {
		if (touch_y < S2W_SCREEN_ON_Y_MIN) {
			ignore_touch = true;
		} else {
			s2w_handle_sweep2sleep(x, y);
		}
	}
}

static inline void s2w_handle_sweep2sleep(int x, int y) {
	if (touch_state == STATE_TOUCH_1) {

    			if (x > S2W_X_B2) {
#ifdef ENFORCE_X_MAX
				if (x < S2W_X_MAX)
#endif
	    				touch_state = STATE_TOUCH_2;
			}
    			else {
    				ignore_touch = true;
			}
    } else if (touch_state == STATE_TOUCH_2) {
    			if (x > S2W_X_B1 && x < S2W_X_B2)
    				touch_state = STATE_TOUCH_3;
			else if (x < S2W_X_B1) {
    				if (!ignore_touch) {
					ignore_touch = true;
					pr_info(LOGTAG "power off (sweep2sleep)\n");
					sweep2wake_pwrtrigger();
				}
    			} 

	} else if (touch_state == STATE_TOUCH_3) {
    			if (x < S2W_X_B1) {
    				if (!ignore_touch) {
					ignore_touch = true;
					pr_info(LOGTAG "power off (sweep2sleep)\n");
					sweep2wake_pwrtrigger();
				}
    			} else if (x > S2W_X_B2) {
				ignore_touch = true;
			}		
	}
}

static inline void s2w_handle_sweep2wake(int x, int y) {
	if (touch_state == STATE_TOUCH_1) {
	    		if (x < S2W_X_B1) {
		    				touch_state = STATE_TOUCH_2;
				}
	    		 else if (x > S2W_X_B2) {
	    				ignore_touch = true;
				}
	} else if (touch_state == STATE_TOUCH_2) {
    			if (x > S2W_X_B1 && x < S2W_X_B2)
    				touch_state = STATE_TOUCH_3;
				else if (x > S2W_X_B2) {
    				#ifdef ENFORCE_X_MAX
					if (x < S2W_X_MAX)
					#endif
   					if (!ignore_touch) {
						ignore_touch = true;
						pr_info(LOGTAG "power on (sweep2wake)\n");
						sweep2wake_pwrtrigger();
					}
    			}
	} else if (touch_state == STATE_TOUCH_3) {
    			if (x > S2W_X_B2) {
    				#ifdef ENFORCE_X_MAX
					if (x < S2W_X_MAX)
					#endif
    				if (!ignore_touch) {
						ignore_touch = true;
						pr_info(LOGTAG "power on (sweep2wake)\n");
						sweep2wake_pwrtrigger();
					}
    			} else if (x < S2W_X_B1) {
					ignore_touch = true;
				}
	}
}

static inline int s2w_math_abs(int x) {
	return x > 0 ? x : -x;
}

static bool s2w_set_tap_time(void) {
	cputime64_t last_time = tap_time;
	tap_time = ktime_to_ms(ktime_get());
	return (tap_time - last_time) > DT2W_TIME;
}

static inline void s2w_handle_doubletap2wake(int x, int y) {
	if (tap_count == 0 ||
			s2w_math_abs(x - tap_down_x) > DT2W_TOUCHSLOP ||
			s2w_math_abs(y - tap_down_y) > DT2W_TOUCHSLOP) {
		tap_count = 1;
		tap_down_x = x;
		tap_down_y = y;
		tap_time = ktime_to_ms(ktime_get());
	} else  if (s2w_set_tap_time()) {
		tap_down_x = x;
		tap_down_y = y;
	} else {
		pr_info(LOGTAG "power on (doubletap2wake)\n");
		sweep2wake_pwrtrigger();
		doubletap2wake_reset();
	}
	ignore_touch = true;
}

static void s2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if S2W_DEBUG
	pr_info(LOGTAG "ignore_touch=%d  power_triggered=%d  %s (%u) --> value=%d\n",
		ignore_touch, power_triggered,
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		(code == ABS_MT_SLOT) ? "SLOT" :
		"undef"), code, value);
#endif
	if (s2w_switch) {
		//(
		if (code == ABS_MT_TRACKING_ID && value == -1) {
			sweep2wake_reset();
		} else if (!ignore_touch && !power_triggered) {
			if (code == ABS_MT_POSITION_X) {
				touch_x = value;
				touch_x_called = true;
			} else if (code == ABS_MT_POSITION_Y) {
				touch_y = value;
				if (touch_x_called) {
					touch_x_called = false;
					queue_work_on(0, s2w_input_wq, &s2w_input_work);
				}
			}
		}
	}
}

static int input_dev_filter(struct input_dev *dev) {
#if S2W_DEBUG
	pr_info("sweep2wake: device filter: %s ", dev->name);
#endif
	if (!strcmp(dev->name, "sec_touchscreen")) {
		return 0;
	} else {
		return 1;
	}
}

static int s2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "s2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void s2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2w_input_handler = {
	.event		= s2w_input_event,
	.connect	= s2w_input_connect,
	.disconnect	= s2w_input_disconnect,
	.name		= "s2w_inputreq",
	.id_table	= s2w_ids,
};

/*
 * SYSFS stuff below here
 */
static ssize_t s2w_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	char modes[40];
	memset(modes, 0x0, 40);
	if (s2w_switch & SWEEP2WAKE_FLAG_SWEEP_WAKE) {
		strcat(modes, SWEEP2WAKE_LABEL);
	}
	if (s2w_switch & SWEEP2WAKE_FLAG_DOUBLE_TAP) {
		strcat(modes, DOUBLETAP2WAKE_LABEL);
	}
	if (s2w_switch & SWEEP2WAKE_FLAG_SWEEP_SLEEP) {
		if (strlen(modes)) {
			strcat(modes, ", ");
		}
		strcat(modes, SWEEP2SLEEP_LABEL);
	}
	if (!strlen(modes)) {
		strcat(modes, NONE_ENABLED_LABEL);
	}
	count += sprintf(buf, "mode: %d, enabled: %s\n", s2w_switch, modes);
	return count;
}

static ssize_t s2w_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int new_mode = 0;
	if (buf[0] >= '0' && buf[0] <= '7' && buf[1] == '\n') {
		new_mode = buf[0] - '0';
		if (new_mode != s2w_switch) {
			s2w_switch = new_mode;
			sweep2wake_reset();
			doubletap2wake_reset();
		}
	}

	return count;
}

static DEVICE_ATTR(mode, (S_IWUSR|S_IRUGO),
	s2w_sweep2wake_show, s2w_sweep2wake_dump);

static ssize_t s2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t s2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static void sweep2wake_early_suspend(struct early_suspend *h) {
	power_triggered = false;
	scr_suspended = true;
	sweep2wake_reset();
	doubletap2wake_reset();
}

static void sweep2wake_late_resume(struct early_suspend *h) {
	power_triggered = false;
	scr_suspended = false;
	sweep2wake_reset();
}

static struct early_suspend sweep2wake_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 2,
	.suspend = sweep2wake_early_suspend,
	.resume = sweep2wake_late_resume,
};

static DEVICE_ATTR(version, (S_IWUSR|S_IRUGO),
	s2w_version_show, s2w_version_dump);

/*
 * INIT / EXIT stuff below here
 */
//#ifdef ANDROID_TOUCH_DECLARED
//extern struct kobject *android_touch_kobj;
//#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
//#endif
static int __init sweep2wake_init(void)
{
	int rc = 0;

	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);
	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	s2w_input_wq = create_workqueue("s2wiwq");
	if (!s2w_input_wq) {
		pr_err("%s: Failed to create s2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&s2w_input_work, s2w_input_callback);
	rc = input_register_handler(&s2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2w_input_handler\n", __func__);

	register_early_suspend(&sweep2wake_early_suspend_handler);

//#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("power_actions", NULL) ;
	if (android_touch_kobj == NULL) {
		pr_warn("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
//#endif
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_mode.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_version.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake_version\n", __func__);
	}

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG "%s done\n", __func__);

	return 0;
}

static void __exit sweep2wake_exit(void)
{
//#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
//#endif
	input_unregister_handler(&s2w_input_handler);
	destroy_workqueue(s2w_input_wq);
	input_unregister_device(sweep2wake_pwrdev);
	input_free_device(sweep2wake_pwrdev);
	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);
