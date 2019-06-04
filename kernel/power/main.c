/*
 * kernel/power/main.c - PM subsystem core functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released under the GPLv2
 *
 */
/*
 * NOTE: This file has been modified by Sony Mobile Communications Inc.
 * Modifications are Copyright (c) 2014 Sony Mobile Communications Inc,
 * and licensed under the license of the file.
 */

#include <linux/export.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/pm-trace.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#ifdef CONFIG_PM_WAKEUP_TIMES
#include <linux/poll.h>
#endif

#include "power.h"

DEFINE_MUTEX(pm_mutex);

#ifdef CONFIG_PM_SLEEP
/*
 * Enable/Disable suspend back off logic attribute
 */
static bool sbo_enabled = true;

/*
 * For how many percent an alive time is decayed
 * if suspend back-off keeps ongoing.
 */
static u32 sbo_decay_value = 20;

/*
 * Initial max back-off alive time
 */
static u32 sbo_initial_alive_time_msecs = 10000;

/*
 * A time in milliseconds, before which a short
 * sleep counter is increased to trigger a back-off.
 */
static u32 sbo_short_sleep_msecs = 1100;

/*
 * This variable regulates starting of suspend
 * back off functionality, after counter is reached
 * specified value.
 */
static u32 sbo_short_sleep_count = 10;

static u32 decay_alive_time_ms;
static u32 suspend_short_count;
static struct wakeup_source *ws;

/* Routines for PM-transition notifications */

static BLOCKING_NOTIFIER_HEAD(pm_chain_head);

int register_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_pm_notifier);

int unregister_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_pm_notifier);

int __pm_notifier_call_chain(unsigned long val, int nr_to_call, int *nr_calls)
{
	int ret;

	ret = __blocking_notifier_call_chain(&pm_chain_head, val, NULL,
						nr_to_call, nr_calls);

	return notifier_to_errno(ret);
}
int pm_notifier_call_chain(unsigned long val)
{
	return __pm_notifier_call_chain(val, -1, NULL);
}

/* If set, devices may be suspended and resumed asynchronously. */
int pm_async_enabled = 1;

static ssize_t pm_async_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_async_enabled);
}

static ssize_t pm_async_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	pm_async_enabled = val;
	return n;
}

power_attr(pm_async);

#ifdef CONFIG_SUSPEND
static ssize_t mem_sleep_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	char *s = buf;
	suspend_state_t i;

	for (i = PM_SUSPEND_MIN; i < PM_SUSPEND_MAX; i++)
		if (mem_sleep_states[i]) {
			const char *label = mem_sleep_states[i];

			if (mem_sleep_current == i)
				s += sprintf(s, "[%s] ", label);
			else
				s += sprintf(s, "%s ", label);
		}

	/* Convert the last space to a newline if needed. */
	if (s != buf)
		*(s-1) = '\n';

	return (s - buf);
}

static suspend_state_t decode_suspend_state(const char *buf, size_t n)
{
	suspend_state_t state;
	char *p;
	int len;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	for (state = PM_SUSPEND_MIN; state < PM_SUSPEND_MAX; state++) {
		const char *label = mem_sleep_states[state];

		if (label && len == strlen(label) && !strncmp(buf, label, len))
			return state;
	}

	return PM_SUSPEND_ON;
}

static ssize_t mem_sleep_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t n)
{
	suspend_state_t state;
	int error;

	error = pm_autosleep_lock();
	if (error)
		return error;

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

	state = decode_suspend_state(buf, n);
	if (state < PM_SUSPEND_MAX && state > PM_SUSPEND_ON)
		mem_sleep_current = state;
	else
		error = -EINVAL;

 out:
	pm_autosleep_unlock();
	return error ? error : n;
}

power_attr(mem_sleep);
#endif /* CONFIG_SUSPEND */

#ifdef CONFIG_PM_SLEEP_DEBUG
int pm_test_level = TEST_NONE;

static const char * const pm_tests[__TEST_AFTER_LAST] = {
	[TEST_NONE] = "none",
	[TEST_CORE] = "core",
	[TEST_CPUS] = "processors",
	[TEST_PLATFORM] = "platform",
	[TEST_DEVICES] = "devices",
	[TEST_FREEZER] = "freezer",
};

static ssize_t pm_test_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	char *s = buf;
	int level;

	for (level = TEST_FIRST; level <= TEST_MAX; level++)
		if (pm_tests[level]) {
			if (level == pm_test_level)
				s += sprintf(s, "[%s] ", pm_tests[level]);
			else
				s += sprintf(s, "%s ", pm_tests[level]);
		}

	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';

	return (s - buf);
}

static ssize_t pm_test_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	const char * const *s;
	int level;
	char *p;
	int len;
	int error = -EINVAL;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	lock_system_sleep();

	level = TEST_FIRST;
	for (s = &pm_tests[level]; level <= TEST_MAX; s++, level++)
		if (*s && len == strlen(*s) && !strncmp(buf, *s, len)) {
			pm_test_level = level;
			error = 0;
			break;
		}

	unlock_system_sleep();

	return error ? error : n;
}

power_attr(pm_test);
#endif /* CONFIG_PM_SLEEP_DEBUG */

#ifdef CONFIG_DEBUG_FS
static char *suspend_step_name(enum suspend_stat_step step)
{
	switch (step) {
	case SUSPEND_FREEZE:
		return "freeze";
	case SUSPEND_PREPARE:
		return "prepare";
	case SUSPEND_SUSPEND:
		return "suspend";
	case SUSPEND_SUSPEND_NOIRQ:
		return "suspend_noirq";
	case SUSPEND_RESUME_NOIRQ:
		return "resume_noirq";
	case SUSPEND_RESUME:
		return "resume";
	default:
		return "";
	}
}

static int suspend_stats_show(struct seq_file *s, void *unused)
{
	int i, index, last_dev, last_errno, last_step;

	last_dev = suspend_stats.last_failed_dev + REC_FAILED_NUM - 1;
	last_dev %= REC_FAILED_NUM;
	last_errno = suspend_stats.last_failed_errno + REC_FAILED_NUM - 1;
	last_errno %= REC_FAILED_NUM;
	last_step = suspend_stats.last_failed_step + REC_FAILED_NUM - 1;
	last_step %= REC_FAILED_NUM;
	seq_printf(s, "%s: %d\n%s: %d\n%s: %d\n%s: %d\n%s: %d\n"
			"%s: %d\n%s: %d\n%s: %d\n%s: %d\n%s: %d\n",
			"success", suspend_stats.success,
			"fail", suspend_stats.fail,
			"failed_freeze", suspend_stats.failed_freeze,
			"failed_prepare", suspend_stats.failed_prepare,
			"failed_suspend", suspend_stats.failed_suspend,
			"failed_suspend_late",
				suspend_stats.failed_suspend_late,
			"failed_suspend_noirq",
				suspend_stats.failed_suspend_noirq,
			"failed_resume", suspend_stats.failed_resume,
			"failed_resume_early",
				suspend_stats.failed_resume_early,
			"failed_resume_noirq",
				suspend_stats.failed_resume_noirq);
	seq_printf(s,	"failures:\n  last_failed_dev:\t%-s\n",
			suspend_stats.failed_devs[last_dev]);
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_dev + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-s\n",
			suspend_stats.failed_devs[index]);
	}
	seq_printf(s,	"  last_failed_errno:\t%-d\n",
			suspend_stats.errno[last_errno]);
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_errno + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-d\n",
			suspend_stats.errno[index]);
	}
	seq_printf(s,	"  last_failed_step:\t%-s\n",
			suspend_step_name(
				suspend_stats.failed_steps[last_step]));
	for (i = 1; i < REC_FAILED_NUM; i++) {
		index = last_step + REC_FAILED_NUM - i;
		index %= REC_FAILED_NUM;
		seq_printf(s, "\t\t\t%-s\n",
			suspend_step_name(
				suspend_stats.failed_steps[index]));
	}

#ifdef CONFIG_PM_WAKEUP_TIMES
	seq_printf(s,	"%s\n%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms\n",
		"suspend time:",
		"  min:", ktime_to_ms(ktime_sub(
			suspend_stats.suspend_min_time.end,
			suspend_stats.suspend_min_time.start)),
		"start:", ktime_to_ms(suspend_stats.suspend_min_time.start),
		"end:", ktime_to_ms(suspend_stats.suspend_min_time.end),
		"  max:", ktime_to_ms(ktime_sub(
			suspend_stats.suspend_max_time.end,
			suspend_stats.suspend_max_time.start)),
		"start:", ktime_to_ms(suspend_stats.suspend_max_time.start),
		"end:", ktime_to_ms(suspend_stats.suspend_max_time.end),
		"  last:", ktime_to_ms(ktime_sub(
			suspend_stats.suspend_last_time.end,
			suspend_stats.suspend_last_time.start)),
		"start:", ktime_to_ms(suspend_stats.suspend_last_time.start),
		"end:", ktime_to_ms(suspend_stats.suspend_last_time.end),
		"  avg:", ktime_to_ms(suspend_stats.suspend_avg_time));

	seq_printf(s,	"%s\n%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms (%s %lldms %s %lldms)\n" \
			"%s  %lldms\n",
		"resume time:",
		"  min:", ktime_to_ms(ktime_sub(
			suspend_stats.resume_min_time.end,
			suspend_stats.resume_min_time.start)),
		"start:", ktime_to_ms(suspend_stats.resume_min_time.start),
		"end:", ktime_to_ms(suspend_stats.resume_min_time.end),
		"  max:", ktime_to_ms(ktime_sub(
			suspend_stats.resume_max_time.end,
			suspend_stats.resume_max_time.start)),
		"start:", ktime_to_ms(suspend_stats.resume_max_time.start),
		"end:", ktime_to_ms(suspend_stats.resume_max_time.end),
		"  last:", ktime_to_ms(ktime_sub(
			suspend_stats.resume_last_time.end,
			suspend_stats.resume_last_time.start)),
		"start:", ktime_to_ms(suspend_stats.resume_last_time.start),
		"end:", ktime_to_ms(suspend_stats.resume_last_time.end),
		"  avg:", ktime_to_ms(suspend_stats.resume_avg_time));
#endif
	return 0;
}

static int suspend_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, suspend_stats_show, NULL);
}

#ifdef CONFIG_PM_WAKEUP_TIMES
static unsigned int suspend_stats_poll(struct file *filp,
			struct poll_table_struct *wait)
{
	unsigned int mask = 0;

	poll_wait(filp, &suspend_stats_queue.wait_queue, wait);
	if (suspend_stats_queue.resume_done) {
		mask |= (POLLIN | POLLRDNORM);
		suspend_stats_queue.resume_done = 0;
	}

	return mask;
}
#endif

static const struct file_operations suspend_stats_operations = {
	.open           = suspend_stats_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
#ifdef CONFIG_PM_WAKEUP_TIMES
	.poll           = suspend_stats_poll,
#endif
	.release        = single_release,
};

static int __init pm_debugfs_init(void)
{
	debugfs_create_file("suspend_stats", S_IFREG | S_IRUGO,
			NULL, NULL, &suspend_stats_operations);
#ifdef CONFIG_PM_WAKEUP_TIMES
	init_waitqueue_head(&suspend_stats_queue.wait_queue);
#endif
	return 0;
}

late_initcall(pm_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_SLEEP_DEBUG
/*
 * pm_print_times: print time taken by devices to suspend and resume.
 *
 * show() returns whether printing of suspend and resume times is enabled.
 * store() accepts 0 or 1.  0 disables printing and 1 enables it.
 */
bool pm_print_times_enabled;

static ssize_t pm_print_times_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pm_print_times_enabled);
}

static ssize_t pm_print_times_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	pm_print_times_enabled = !!val;
	return n;
}

power_attr(pm_print_times);

static inline void pm_print_times_init(void)
{
	pm_print_times_enabled = !!initcall_debug;
}

static ssize_t pm_wakeup_irq_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return pm_wakeup_irq ? sprintf(buf, "%u\n", pm_wakeup_irq) : -ENODATA;
}

power_attr_ro(pm_wakeup_irq);

bool pm_debug_messages_on __read_mostly;

static ssize_t pm_debug_messages_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pm_debug_messages_on);
}

static ssize_t pm_debug_messages_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	pm_debug_messages_on = !!val;
	return n;
}

power_attr(pm_debug_messages);

/**
 * __pm_pr_dbg - Print a suspend debug message to the kernel log.
 * @defer: Whether or not to use printk_deferred() to print the message.
 * @fmt: Message format.
 *
 * The message will be emitted if enabled through the pm_debug_messages
 * sysfs attribute.
 */
void __pm_pr_dbg(bool defer, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (!pm_debug_messages_on)
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (defer)
		printk_deferred(KERN_DEBUG "PM: %pV", &vaf);
	else
		printk(KERN_DEBUG "PM: %pV", &vaf);

	va_end(args);
}

#else /* !CONFIG_PM_SLEEP_DEBUG */
static inline void pm_print_times_init(void) {}
#endif /* CONFIG_PM_SLEEP_DEBUG */

struct kobject *power_kobj;

/**
 * state - control system sleep states.
 *
 * show() returns available sleep state labels, which may be "mem", "standby",
 * "freeze" and "disk" (hibernation).  See Documentation/power/states.txt for a
 * description of what they mean.
 *
 * store() accepts one of those strings, translates it into the proper
 * enumerated value, and initiates a suspend transition.
 */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	char *s = buf;
#ifdef CONFIG_SUSPEND
	suspend_state_t i;

	for (i = PM_SUSPEND_MIN; i < PM_SUSPEND_MAX; i++)
		if (pm_states[i])
			s += sprintf(s,"%s ", pm_states[i]);

#endif
	if (hibernation_available())
		s += sprintf(s, "disk ");
	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';
	return (s - buf);
}

static suspend_state_t decode_state(const char *buf, size_t n)
{
#ifdef CONFIG_SUSPEND
	suspend_state_t state;
#endif
	char *p;
	int len;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	/* Check hibernation first. */
	if (len == 4 && !strncmp(buf, "disk", len))
		return PM_SUSPEND_MAX;

#ifdef CONFIG_SUSPEND
	for (state = PM_SUSPEND_MIN; state < PM_SUSPEND_MAX; state++) {
		const char *label = pm_states[state];

		if (label && len == strlen(label) && !strncmp(buf, label, len))
			return state;
	}
#endif

	return PM_SUSPEND_ON;
}

static inline u32
decay_val(u32 val, u32 percent)
{
	u32 ratio;

	percent = clamp(percent, 0U, 100U);
	ratio = 1024 - ((1024 * percent) / 100);

	return (val * ratio) / 1024;
}

static ssize_t sbo_decay_value_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 100)
		return -EINVAL;

	sbo_decay_value = val;
	return n;
}

static ssize_t sbo_decay_value_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", sbo_decay_value);
}

power_attr(sbo_decay_value);

static ssize_t sbo_short_sleep_msecs_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	sbo_short_sleep_msecs = val;
	return n;
}

static ssize_t sbo_short_sleep_msecs_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", sbo_short_sleep_msecs);
}

power_attr(sbo_short_sleep_msecs);

static ssize_t sbo_short_sleep_count_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	sbo_short_sleep_count = val;
	return n;
}

static ssize_t sbo_short_sleep_count_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", sbo_short_sleep_count);
}

power_attr(sbo_short_sleep_count);

static ssize_t sbo_initial_alive_time_msecs_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	sbo_initial_alive_time_msecs = val;
	return n;
}

static ssize_t sbo_initial_alive_time_msecs_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", sbo_initial_alive_time_msecs);
}

power_attr(sbo_initial_alive_time_msecs);

static ssize_t sbo_enabled_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	sbo_enabled = !!val;
	return n;
}

static ssize_t sbo_enabled_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sbo_enabled);
}

power_attr(sbo_enabled);

static void
suspend_backoff(u32 timeout_msecs)
{
	if (!sbo_enabled)
		return;

	pr_info("suspend: too many immediate wakeups, back off (%u msecs)\n",
			timeout_msecs);

	__pm_wakeup_event(ws, timeout_msecs);
}

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t n)
{
	suspend_state_t state;
	struct timespec ts_entry, ts_exit;
	u64 elapsed_msecs64;
	u32 elapsed_msecs32;
	int error;

	error = pm_autosleep_lock();
	if (error)
		return error;

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

	state = decode_state(buf, n);
	if (state < PM_SUSPEND_MAX) {
		if (state == PM_SUSPEND_MEM)
			state = mem_sleep_current;

		/*
		 * We want to prevent system from frequent periodic wake-ups
		 * when sleeping time is less or equal certain interval.
		 * It's done in order to save power in certain cases, one of
		 * the examples is GPS tracking, but not only.
		 */
		getnstimeofday(&ts_entry);
		error = pm_suspend(state);
		getnstimeofday(&ts_exit);

		elapsed_msecs64 = timespec_to_ns(&ts_exit) -
			timespec_to_ns(&ts_entry);
		do_div(elapsed_msecs64, NSEC_PER_MSEC);
		elapsed_msecs32 = elapsed_msecs64;

		if (elapsed_msecs32 <= sbo_short_sleep_msecs) {
			if (suspend_short_count == sbo_short_sleep_count) {
				if (decay_alive_time_ms >= MSEC_PER_SEC) {
					suspend_backoff(decay_alive_time_ms);
					decay_alive_time_ms =
						decay_val(decay_alive_time_ms,
							sbo_decay_value);
					goto out;
				}
			} else {
				suspend_short_count++;
				goto out;
			}
		}

		/* Start from scratch */
		suspend_short_count = 0;

		/*
		 * Randomize a bit an initial alive time value to be
		 * not synced with any wake up sources, for example
		 * IRQs.
		 */
		decay_alive_time_ms = sbo_initial_alive_time_msecs -
			get_random_int() % (sbo_initial_alive_time_msecs / 10);
	} else if (state == PM_SUSPEND_MAX) {
		error = hibernate();
	} else {
		error = -EINVAL;
	}

 out:
	pm_autosleep_unlock();
	return error ? error : n;
}

power_attr(state);

#ifdef CONFIG_PM_SLEEP
/*
 * The 'wakeup_count' attribute, along with the functions defined in
 * drivers/base/power/wakeup.c, provides a means by which wakeup events can be
 * handled in a non-racy way.
 *
 * If a wakeup event occurs when the system is in a sleep state, it simply is
 * woken up.  In turn, if an event that would wake the system up from a sleep
 * state occurs when it is undergoing a transition to that sleep state, the
 * transition should be aborted.  Moreover, if such an event occurs when the
 * system is in the working state, an attempt to start a transition to the
 * given sleep state should fail during certain period after the detection of
 * the event.  Using the 'state' attribute alone is not sufficient to satisfy
 * these requirements, because a wakeup event may occur exactly when 'state'
 * is being written to and may be delivered to user space right before it is
 * frozen, so the event will remain only partially processed until the system is
 * woken up by another event.  In particular, it won't cause the transition to
 * a sleep state to be aborted.
 *
 * This difficulty may be overcome if user space uses 'wakeup_count' before
 * writing to 'state'.  It first should read from 'wakeup_count' and store
 * the read value.  Then, after carrying out its own preparations for the system
 * transition to a sleep state, it should write the stored value to
 * 'wakeup_count'.  If that fails, at least one wakeup event has occurred since
 * 'wakeup_count' was read and 'state' should not be written to.  Otherwise, it
 * is allowed to write to 'state', but the transition will be aborted if there
 * are any wakeup events detected after 'wakeup_count' was written to.
 */

static ssize_t wakeup_count_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	unsigned int val;

	return pm_get_wakeup_count(&val, true) ?
		sprintf(buf, "%u\n", val) : -EINTR;
}

static ssize_t wakeup_count_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned int val;
	int error;

	error = pm_autosleep_lock();
	if (error)
		return error;

	if (pm_autosleep_state() > PM_SUSPEND_ON) {
		error = -EBUSY;
		goto out;
	}

	error = -EINVAL;
	if (sscanf(buf, "%u", &val) == 1) {
		if (pm_save_wakeup_count(val))
			error = n;
		else
			pm_print_active_wakeup_sources();
	}

 out:
	pm_autosleep_unlock();
	return error;
}

power_attr(wakeup_count);

#ifdef CONFIG_PM_AUTOSLEEP
static ssize_t autosleep_show(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	suspend_state_t state = pm_autosleep_state();

	if (state == PM_SUSPEND_ON)
		return sprintf(buf, "off\n");

#ifdef CONFIG_SUSPEND
	if (state < PM_SUSPEND_MAX)
		return sprintf(buf, "%s\n", pm_states[state] ?
					pm_states[state] : "error");
#endif
#ifdef CONFIG_HIBERNATION
	return sprintf(buf, "disk\n");
#else
	return sprintf(buf, "error");
#endif
}

static ssize_t autosleep_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t n)
{
	suspend_state_t state = decode_state(buf, n);
	int error;

	if (state == PM_SUSPEND_ON
	    && strcmp(buf, "off") && strcmp(buf, "off\n"))
		return -EINVAL;

	if (state == PM_SUSPEND_MEM)
		state = mem_sleep_current;

	error = pm_autosleep_set_state(state);
	return error ? error : n;
}

power_attr(autosleep);
#endif /* CONFIG_PM_AUTOSLEEP */

#ifdef CONFIG_PM_WAKELOCKS
static ssize_t wake_lock_show(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	return pm_show_wakelocks(buf, true);
}

static ssize_t wake_lock_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t n)
{
	int error = pm_wake_lock(buf);
	return error ? error : n;
}

power_attr(wake_lock);

static ssize_t wake_unlock_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	return pm_show_wakelocks(buf, false);
}

static ssize_t wake_unlock_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t n)
{
	int error = pm_wake_unlock(buf);
	return error ? error : n;
}

power_attr(wake_unlock);

#endif /* CONFIG_PM_WAKELOCKS */
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_TRACE
int pm_trace_enabled;

static ssize_t pm_trace_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_trace_enabled);
}

static ssize_t
pm_trace_store(struct kobject *kobj, struct kobj_attribute *attr,
	       const char *buf, size_t n)
{
	int val;

	if (sscanf(buf, "%d", &val) == 1) {
		pm_trace_enabled = !!val;
		if (pm_trace_enabled) {
			pr_warn("PM: Enabling pm_trace changes system date and time during resume.\n"
				"PM: Correct system time has to be restored manually after resume.\n");
		}
		return n;
	}
	return -EINVAL;
}

power_attr(pm_trace);

static ssize_t pm_trace_dev_match_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return show_trace_dev_match(buf, PAGE_SIZE);
}

power_attr_ro(pm_trace_dev_match);

#endif /* CONFIG_PM_TRACE */

#ifdef CONFIG_FREEZER
static ssize_t pm_freeze_timeout_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", freeze_timeout_msecs);
}

static ssize_t pm_freeze_timeout_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t n)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	freeze_timeout_msecs = val;
	return n;
}

power_attr(pm_freeze_timeout);

#endif	/* CONFIG_FREEZER*/

static struct attribute * g[] = {
	&state_attr.attr,
#ifdef CONFIG_PM_TRACE
	&pm_trace_attr.attr,
	&pm_trace_dev_match_attr.attr,
#endif
#ifdef CONFIG_PM_SLEEP
	&pm_async_attr.attr,
	&wakeup_count_attr.attr,
#ifdef CONFIG_SUSPEND
	&mem_sleep_attr.attr,
#endif
	&sbo_enabled_attr.attr,
	&sbo_decay_value_attr.attr,
	&sbo_short_sleep_msecs_attr.attr,
	&sbo_short_sleep_count_attr.attr,
	&sbo_initial_alive_time_msecs_attr.attr,
#ifdef CONFIG_PM_AUTOSLEEP
	&autosleep_attr.attr,
#endif
#ifdef CONFIG_PM_WAKELOCKS
	&wake_lock_attr.attr,
	&wake_unlock_attr.attr,
#endif
#ifdef CONFIG_PM_SLEEP_DEBUG
	&pm_test_attr.attr,
	&pm_print_times_attr.attr,
	&pm_wakeup_irq_attr.attr,
	&pm_debug_messages_attr.attr,
#endif
#endif
#ifdef CONFIG_FREEZER
	&pm_freeze_timeout_attr.attr,
#endif
	NULL,
};

static const struct attribute_group attr_group = {
	.attrs = g,
};

struct workqueue_struct *pm_wq;
EXPORT_SYMBOL_GPL(pm_wq);

static int __init pm_start_workqueue(void)
{
	pm_wq = alloc_workqueue("pm", WQ_FREEZABLE | WQ_MEM_RECLAIM, 0);

	return pm_wq ? 0 : -ENOMEM;
}

static int __init pm_init(void)
{
	int error = pm_start_workqueue();
	if (error)
		return error;
	hibernate_image_size_init();
	hibernate_reserved_size_init();
	pm_states_init();
	power_kobj = kobject_create_and_add("power", NULL);
	if (!power_kobj)
		return -ENOMEM;
	error = sysfs_create_group(power_kobj, &attr_group);
	if (error)
		return error;
	pm_print_times_init();

#ifdef CONFIG_PM_SLEEP
	ws = wakeup_source_register("suspend_backoff");
#endif
	return pm_autosleep_init();
}

core_initcall(pm_init);
