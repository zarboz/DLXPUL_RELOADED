/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "subsys-restart: %s(): " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/wakelock.h>
#include <linux/suspend.h>

#include <asm/current.h>

#include <mach/peripheral-loader.h>
#include <mach/scm.h>
#include <mach/socinfo.h>
#include <mach/subsystem_notif.h>
#include <mach/subsystem_restart.h>
#include <mach/board_htc.h>
#include "qsc_dsda.h"

#include "smd_private.h"
#include <mach/htc_restart_handler.h>


#if defined(CONFIG_ARCH_APQ8064)
  #define EXTERNAL_MODEM "external_modem"
  #define SZ_DIAG_ERR_MSG 	0xC8

  extern char *get_mdm_errmsg(void);
#endif


#if defined(pr_debug)
#undef pr_debug
#endif
#define pr_debug(x...) do {				\
			printk(KERN_DEBUG "[SSR] "x);		\
	} while (0)

#if defined(pr_warning)
#undef pr_warning
#endif
#define pr_warning(x...) do {				\
			printk(KERN_WARNING "[SSR] "x);		\
	} while (0)

#if defined(pr_info)
#undef pr_info
#endif
#define pr_info(x...) do {				\
			printk(KERN_INFO "[SSR] "x);		\
	} while (0)

#if defined(pr_err)
#undef pr_err
#endif
#define pr_err(x...) do {				\
			printk(KERN_ERR "[SSR] "x);		\
	} while (0)

struct subsys_soc_restart_order {
	const char * const *subsystem_list;
	int count;

	struct mutex shutdown_lock;
	struct mutex powerup_lock;
	struct subsys_data *subsys_ptrs[];
};

struct restart_wq_data {
	struct subsys_data *subsys;
	struct wake_lock ssr_wake_lock;
	char wlname[64];
	int use_restart_order;
	struct work_struct work;
};

struct restart_log {
	struct timeval time;
	struct subsys_data *subsys;
	struct list_head list;
};

static int restart_level;
static int enable_ramdumps;
struct workqueue_struct *ssr_wq;
#ifdef CONFIG_QSC_MODEM
static int crashed_modem;
#endif

#if defined(CONFIG_ARCH_APQ8064) && defined(CONFIG_USB_EHCI_MSM_HSIC)
int mdm_is_in_restart = 0;
#endif 


static LIST_HEAD(restart_log_list);
static LIST_HEAD(subsystem_list);
static DEFINE_SPINLOCK(subsystem_list_lock);
static DEFINE_MUTEX(soc_order_reg_lock);
static DEFINE_MUTEX(restart_log_mutex);


#define DEFINE_SINGLE_RESTART_ORDER(name, order)		\
	static struct subsys_soc_restart_order __##name = {	\
		.subsystem_list = order,			\
		.count = ARRAY_SIZE(order),			\
		.subsys_ptrs = {[ARRAY_SIZE(order)] = NULL}	\
	};							\
	static struct subsys_soc_restart_order *name[] = {      \
		&__##name,					\
	}

static const char * const _order_8x60_all[] = {
	"external_modem",  "modem", "lpass"
};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_all, _order_8x60_all);

static const char * const _order_8x60_modems[] = {"external_modem", "modem"};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_modems, _order_8x60_modems);

static const char * const order_8960[] = {"modem", "lpass"};

static const char * const order_8960_sglte[] = {"external_modem",
						"modem"};

static const char * const order_8064_dsda[] = {"external_modem",
						"qsc_modem"};

static struct subsys_soc_restart_order restart_orders_8960_one = {
	.subsystem_list = order_8960,
	.count = ARRAY_SIZE(order_8960),
	.subsys_ptrs = {[ARRAY_SIZE(order_8960)] = NULL}
	};

static struct subsys_soc_restart_order restart_orders_8960_fusion_sglte = {
	.subsystem_list = order_8960_sglte,
	.count = ARRAY_SIZE(order_8960_sglte),
	.subsys_ptrs = {[ARRAY_SIZE(order_8960_sglte)] = NULL}
	};

static struct subsys_soc_restart_order restart_orders_8064_fusion_dsda = {
	.subsystem_list = order_8064_dsda,
	.count = ARRAY_SIZE(order_8064_dsda),
	.subsys_ptrs = {[ARRAY_SIZE(order_8064_dsda)] = NULL}
	};

static struct subsys_soc_restart_order *restart_orders_8960[] = {
	&restart_orders_8960_one,
	};

static struct subsys_soc_restart_order *restart_orders_8960_sglte[] = {
	&restart_orders_8960_fusion_sglte,
	};

static struct subsys_soc_restart_order *restart_orders_8064_dsda[] = {
	&restart_orders_8064_fusion_dsda,
	};

static struct subsys_soc_restart_order **restart_orders;
static int n_restart_orders;

module_param(enable_ramdumps, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_QSC_MODEM
module_param(crashed_modem, int, S_IRUGO);
#endif

static struct subsys_soc_restart_order *_update_restart_order(
		struct subsys_data *subsys);

int get_restart_level()
{
	return restart_level;
}
EXPORT_SYMBOL(get_restart_level);

int get_enable_ramdumps()
{
	return enable_ramdumps;
}
EXPORT_SYMBOL(get_enable_ramdumps);

static int restart_level_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = restart_level;

	if (cpu_is_msm9615()) {
		pr_err("Only Phase 1 subsystem restart is supported\n");
		return -EINVAL;
	}

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	if (restart_level == RESET_SUBSYS_INDEPENDENT && is_qsc_dsda()) {
		pr_info("%s: QSC_DSDA need to reset MDM&QSC togother, \
modify restart_level to RESET_SUBSYS_COUPLED\n", __func__ );
		restart_level = RESET_SUBSYS_COUPLED;
	}

	switch (restart_level) {
		case RESET_SOC:
		case RESET_SUBSYS_COUPLED:
		case RESET_SUBSYS_INDEPENDENT:
			pr_info("Phase %d behavior activated.\n", restart_level);
		break;

		default:
			restart_level = old_val;
			return -EINVAL;
		break;
	}
	return 0;
}

module_param_call(restart_level, restart_level_set, param_get_int,
			&restart_level, 0644);

void subsystem_update_restart_level_for_crc(void)
{
#if defined(CONFIG_MSM_SSR_INDEPENDENT)
	pr_info("%s: Default SSR is Enabled...\n", __func__);
#else
	if (board_mfg_mode() || (get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM))
		
		restart_level = RESET_SOC;
	else if (is_qsc_dsda())
		restart_level = RESET_SUBSYS_COUPLED;
	else
		restart_level = RESET_SUBSYS_INDEPENDENT;

	pr_info("%s: Phase %d behavior activated.\n", __func__, restart_level);
#endif
}
EXPORT_SYMBOL(subsystem_update_restart_level_for_crc);

static struct subsys_data *_find_subsystem(const char *subsys_name)
{
	struct subsys_data *subsys;
	unsigned long flags;

	spin_lock_irqsave(&subsystem_list_lock, flags);
	list_for_each_entry(subsys, &subsystem_list, list)
		if (!strncmp(subsys->name, subsys_name,
				SUBSYS_NAME_MAX_LENGTH)) {
			spin_unlock_irqrestore(&subsystem_list_lock, flags);
			return subsys;
		}
	spin_unlock_irqrestore(&subsystem_list_lock, flags);

	return NULL;
}

static struct subsys_soc_restart_order *_update_restart_order(
		struct subsys_data *subsys)
{
	int i, j;

	if (!subsys)
		return NULL;

	if (!subsys->name)
		return NULL;

	mutex_lock(&soc_order_reg_lock);
	for (j = 0; j < n_restart_orders; j++) {
		for (i = 0; i < restart_orders[j]->count; i++)
			if (!strncmp(restart_orders[j]->subsystem_list[i],
				subsys->name, SUBSYS_NAME_MAX_LENGTH)) {

					restart_orders[j]->subsys_ptrs[i] =
						subsys;
					mutex_unlock(&soc_order_reg_lock);
					return restart_orders[j];
			}
	}

	mutex_unlock(&soc_order_reg_lock);

	return NULL;
}

static void _send_notification_to_order(struct subsys_data
			**restart_list, int count,
			enum subsys_notif_type notif_type)
{
	int i;

	for (i = 0; i < count; i++)
		if (restart_list[i])
			subsys_notif_queue_notification(
				restart_list[i]->notif_handle, notif_type);
}

static int max_restarts;
module_param(max_restarts, int, 0644);

static long max_history_time = 3600;
module_param(max_history_time, long, 0644);

static void do_epoch_check(struct subsys_data *subsys)
{
	int n = 0;
	struct timeval *time_first = NULL, *curr_time;
	struct restart_log *r_log, *temp;
	static int max_restarts_check;
	static long max_history_time_check;

	mutex_lock(&restart_log_mutex);

	max_restarts_check = max_restarts;
	max_history_time_check = max_history_time;

	
	if (!max_restarts_check)
		goto out;

	r_log = kmalloc(sizeof(struct restart_log), GFP_KERNEL);
	if (!r_log)
		goto out;
	r_log->subsys = subsys;
	do_gettimeofday(&r_log->time);
	curr_time = &r_log->time;
	INIT_LIST_HEAD(&r_log->list);

	list_add_tail(&r_log->list, &restart_log_list);

	list_for_each_entry_safe(r_log, temp, &restart_log_list, list) {

		if ((curr_time->tv_sec - r_log->time.tv_sec) >
				max_history_time_check) {

			pr_debug("Deleted node with restart_time = %ld\n",
					r_log->time.tv_sec);
			list_del(&r_log->list);
			kfree(r_log);
			continue;
		}
		if (!n) {
			time_first = &r_log->time;
			pr_debug("Time_first: %ld\n", time_first->tv_sec);
		}
		n++;
		pr_debug("Restart_time: %ld\n", r_log->time.tv_sec);
	}

	if (time_first && n >= max_restarts_check) {
		if ((curr_time->tv_sec - time_first->tv_sec) <
				max_history_time_check)
			panic("Subsystems have crashed %d times in less than "
				"%ld seconds!", max_restarts_check,
				max_history_time_check);
	}

out:
	mutex_unlock(&restart_log_mutex);
}

static void subsystem_restart_wq_func(struct work_struct *work)
{
	struct restart_wq_data *r_work = container_of(work,
						struct restart_wq_data, work);
	struct subsys_data **restart_list;
	struct subsys_data *subsys = r_work->subsys;
	struct subsys_soc_restart_order *soc_restart_order = NULL;

	struct mutex *powerup_lock;
	struct mutex *shutdown_lock;

	int i;
	int restart_list_count = 0;

	if (r_work->use_restart_order)
		soc_restart_order = subsys->restart_order;

	if (!soc_restart_order) {
		restart_list = subsys->single_restart_list;
		restart_list_count = 1;
		powerup_lock = &subsys->powerup_lock;
		shutdown_lock = &subsys->shutdown_lock;
	} else {
		restart_list = soc_restart_order->subsys_ptrs;
		restart_list_count = soc_restart_order->count;
		powerup_lock = &soc_restart_order->powerup_lock;
		shutdown_lock = &soc_restart_order->shutdown_lock;
	}

	pr_debug("[%p]: Attempting to get shutdown lock!\n", current);

	if (!mutex_trylock(shutdown_lock))
		goto out;

	pr_debug("[%p]: Attempting to get powerup lock!\n", current);

	if (!mutex_trylock(powerup_lock))
		panic("%s[%p]: Subsystem died during powerup!",
						__func__, current);

	do_epoch_check(subsys);

	mutex_lock(&soc_order_reg_lock);

	pr_debug("[%p]: Starting restart sequence for %s\n", current,
			r_work->subsys->name);

	
	#if defined(CONFIG_ARCH_APQ8064) && defined(CONFIG_USB_EHCI_MSM_HSIC)
	for (i = 0; i < restart_list_count; i++) {
		if (!restart_list[i])
			continue;

		if (strcmp(restart_list[i]->name, EXTERNAL_MODEM) == 0) {
			mdm_is_in_restart = 1;
			pr_debug("[%s]: mdm_is_in_restart=%d\n", __func__, mdm_is_in_restart);
		}
	}
	#endif 
	

	_send_notification_to_order(restart_list,
				restart_list_count,
				SUBSYS_BEFORE_SHUTDOWN);

	for (i = 0; i < restart_list_count; i++) {

		if (!restart_list[i])
			continue;

		pr_info("[%p]: Shutting down %s\n", current,
			restart_list[i]->name);

		if (restart_list[i]->shutdown(restart_list[i]) < 0)
			panic("subsys-restart: %s[%p]: Failed to shutdown %s!",
				__func__, current, restart_list[i]->name);
	}

	_send_notification_to_order(restart_list, restart_list_count,
				SUBSYS_AFTER_SHUTDOWN);

	mutex_unlock(shutdown_lock);

	
	for (i = 0; i < restart_list_count; i++) {
		if (!restart_list[i])
			continue;

		if (restart_list[i]->ramdump)
			if (restart_list[i]->ramdump(enable_ramdumps,
						restart_list[i]) < 0)
				pr_warn("%s[%p]: Ramdump failed.\n",
						restart_list[i]->name, current);
	}

	_send_notification_to_order(restart_list,
			restart_list_count,
			SUBSYS_BEFORE_POWERUP);

	for (i = restart_list_count - 1; i >= 0; i--) {

		if (!restart_list[i])
			continue;

		pr_info("[%p]: Powering up %s\n", current,
					restart_list[i]->name);

		if (restart_list[i]->powerup(restart_list[i]) < 0)
			panic("%s[%p]: Failed to powerup %s!", __func__,
				current, restart_list[i]->name);
	}

	_send_notification_to_order(restart_list,
				restart_list_count,
				SUBSYS_AFTER_POWERUP);

	pr_info("[%p]: Restart sequence for %s completed.\n",
			current, r_work->subsys->name);

	
	#if defined(CONFIG_ARCH_APQ8064) && defined(CONFIG_USB_EHCI_MSM_HSIC)
	for (i = 0; i < restart_list_count; i++) {
		if (!restart_list[i])
			continue;

		if (strcmp(restart_list[i]->name, EXTERNAL_MODEM) == 0) {
			mdm_is_in_restart = 0;
			pr_debug("[%s]: mdm_is_in_restart=%d\n", __func__, mdm_is_in_restart);
		}
	}
	#endif 
	

#ifdef CONFIG_QSC_MODEM
	crashed_modem = 0;
#endif
	mutex_unlock(powerup_lock);

	mutex_unlock(&soc_order_reg_lock);

	pr_debug("[%p]: Released powerup lock!\n", current);

out:
	wake_unlock(&r_work->ssr_wake_lock);
	wake_lock_destroy(&r_work->ssr_wake_lock);
	kfree(r_work);
}

static void __subsystem_restart(struct subsys_data *subsys)
{
	struct restart_wq_data *data = NULL;
	int rc;

	pr_debug("Restarting %s [level=%d]!\n", subsys->name,
				restart_level);

	data = kzalloc(sizeof(struct restart_wq_data), GFP_ATOMIC);
	if (!data)
		panic("%s: Unable to allocate memory to restart %s.",
		      __func__, subsys->name);

	data->subsys = subsys;

	if (restart_level != RESET_SUBSYS_INDEPENDENT)
		data->use_restart_order = 1;

	snprintf(data->wlname, sizeof(data->wlname), "ssr(%s)", subsys->name);
	wake_lock_init(&data->ssr_wake_lock, WAKE_LOCK_SUSPEND, data->wlname);
	wake_lock(&data->ssr_wake_lock);

	INIT_WORK(&data->work, subsystem_restart_wq_func);
	rc = queue_work(ssr_wq, &data->work);
	if (rc < 0)
		panic("%s: Unable to schedule work to restart %s (%d).",
		     __func__, subsys->name, rc);
}

#ifdef CONFIG_SERIAL_MSM_HS_DEBUG_RINGBUFFER
void dump_uart_ringbuffer(void);
#endif
int subsystem_restart(const char *subsys_name)
{
	struct subsys_data *subsys;
	
	#if defined(CONFIG_ARCH_APQ8064) && defined(CONFIG_USB_EHCI_MSM_HSIC)
	extern bool ehci_hsic_is_2nd_enum_done(void);
	#endif 
	

	if (!subsys_name) {
		pr_err("Invalid subsystem name.\n");
		return -EINVAL;
	}

	
	#if defined(CONFIG_ARCH_APQ8064) && defined(CONFIG_USB_EHCI_MSM_HSIC)
	if (strcmp(subsys_name, EXTERNAL_MODEM) == 0) {
		if (!ehci_hsic_is_2nd_enum_done()) {
			pr_err("%s: 2nd enum is not done !!!\n", __func__);
			return -EINVAL;
		}
		else {
			pr_info("%s: 2nd enum is done\n", __func__);
		}
	}
	#endif 
	

	pr_info("Restart sequence requested for %s, restart_level = %d.\n",
		subsys_name, restart_level);

	subsys = _find_subsystem(subsys_name);

	if (!subsys) {
		pr_warn("Unregistered subsystem %s!\n", subsys_name);
		return -EINVAL;
	}

#ifdef CONFIG_QSC_MODEM
	if(strcmp(subsys_name, "external_modem") == 0){
		crashed_modem = 1;
		pr_info("%s set crashed_modem = %d\n", __func__, crashed_modem);
	}
	else if(strcmp(subsys_name, "qsc_modem") == 0){
		crashed_modem = 2;
		pr_info("%s set crashed_modem = %d\n", __func__, crashed_modem);
	}
#endif

#ifdef CONFIG_SERIAL_MSM_HS_DEBUG_RINGBUFFER
	if(strcmp(subsys_name, "qsc_modem") == 0 && enable_ramdumps)
		dump_uart_ringbuffer();
#endif

	switch (restart_level) {

	case RESET_SUBSYS_COUPLED:
	case RESET_SUBSYS_INDEPENDENT:
		__subsystem_restart(subsys);
		break;

	case RESET_SOC:
		
		if(strcmp(subsys_name, "riva") == 0){
			pr_info("%s: %s use its SSR config. Call subsystem_restart directly.\n", __func__, subsys_name);
			__subsystem_restart(subsys);
			break;
		}
#if defined(CONFIG_ARCH_APQ8064)
		if (strcmp(subsys_name, EXTERNAL_MODEM) == 0) {
			char *errmsg = get_mdm_errmsg();

			
			char ramdump_msg[SZ_DIAG_ERR_MSG] = "";
			snprintf(ramdump_msg, (SZ_DIAG_ERR_MSG - 1), "KP: subsys-restart: %s crashed. %s", subsys->name, (errmsg? errmsg: ""));
			set_restart_to_ramdump(ramdump_msg);
			

			panic("subsys-restart: %s crashed. %s", subsys->name, (errmsg? errmsg: ""));
		} else
#endif
		{
			panic("subsys-restart: Resetting the SoC - %s crashed.", subsys->name);
		}
		
		break;

	default:
		panic("subsys-restart: Unknown restart level!\n");
	break;

	}

	return 0;
}
EXPORT_SYMBOL(subsystem_restart);

int ssr_register_subsystem(struct subsys_data *subsys)
{
	unsigned long flags;

	if (!subsys)
		goto err;

	if (!subsys->name)
		goto err;

	if (!subsys->powerup || !subsys->shutdown)
		goto err;

	subsys->notif_handle = subsys_notif_add_subsys(subsys->name);
	subsys->restart_order = _update_restart_order(subsys);
	subsys->single_restart_list[0] = subsys;

	mutex_init(&subsys->shutdown_lock);
	mutex_init(&subsys->powerup_lock);

	spin_lock_irqsave(&subsystem_list_lock, flags);
	list_add(&subsys->list, &subsystem_list);
	spin_unlock_irqrestore(&subsystem_list_lock, flags);

	return 0;

err:
	return -EINVAL;
}
EXPORT_SYMBOL(ssr_register_subsystem);

static int ssr_panic_handler(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	struct subsys_data *subsys;

	list_for_each_entry(subsys, &subsystem_list, list)
		if (subsys->crash_shutdown)
			subsys->crash_shutdown(subsys);
	return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
	.notifier_call  = ssr_panic_handler,
};

static int __init ssr_init_soc_restart_orders(void)
{
	int i;

	atomic_notifier_chain_register(&panic_notifier_list,
			&panic_nb);

	if (cpu_is_msm8x60()) {
		for (i = 0; i < ARRAY_SIZE(orders_8x60_all); i++) {
			mutex_init(&orders_8x60_all[i]->powerup_lock);
			mutex_init(&orders_8x60_all[i]->shutdown_lock);
		}

		for (i = 0; i < ARRAY_SIZE(orders_8x60_modems); i++) {
			mutex_init(&orders_8x60_modems[i]->powerup_lock);
			mutex_init(&orders_8x60_modems[i]->shutdown_lock);
		}

		restart_orders = orders_8x60_all;
		n_restart_orders = ARRAY_SIZE(orders_8x60_all);
	}

	if (cpu_is_msm8960() || cpu_is_msm8930() || cpu_is_msm8930aa() ||
	    cpu_is_msm9615() || cpu_is_apq8064() || cpu_is_msm8627()) {
		if (socinfo_get_platform_subtype() == PLATFORM_SUBTYPE_SGLTE) {
			restart_orders = restart_orders_8960_sglte;
			n_restart_orders =
				ARRAY_SIZE(restart_orders_8960_sglte);
		} else if (socinfo_get_platform_subtype() ==
				   PLATFORM_SUBTYPE_DSDA) {
				restart_orders = restart_orders_8064_dsda;
				n_restart_orders =
					ARRAY_SIZE(restart_orders_8064_dsda);
		} else {
			restart_orders = restart_orders_8960;
			n_restart_orders = ARRAY_SIZE(restart_orders_8960);
		}
		
#if defined(CONFIG_ARCH_DUMMY) || defined(CONFIG_ARCH_DUMMY) || defined(CONFIG_ARCH_DUMMY)
		if (!machine_is_m7_evm()) { 
			restart_orders = restart_orders_8064_dsda;
			n_restart_orders =
				ARRAY_SIZE(restart_orders_8064_dsda);
		}
#endif
		
		for (i = 0; i < n_restart_orders; i++) {
			mutex_init(&restart_orders[i]->powerup_lock);
			mutex_init(&restart_orders[i]->shutdown_lock);
		}
	}

	if (restart_orders == NULL || n_restart_orders < 1) {
		WARN_ON(1);
		return -EINVAL;
	}

	return 0;
}

static int __init subsys_restart_init(void)
{
	int ret = 0;

	
#if defined(CONFIG_MSM_SSR_INDEPENDENT)
	pr_info("%s: Default SSR is Enabled...\n", __func__);

	if (board_mfg_mode() || (get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM))
		restart_level = RESET_SOC;
	else if (is_qsc_dsda())
		restart_level = RESET_SUBSYS_COUPLED;
	else
		restart_level = RESET_SUBSYS_INDEPENDENT;
#else
	pr_info("%s: Default SSR is Disabled...\n", __func__);

	if (!board_mfg_mode() && (get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM)) {
		if (is_qsc_dsda())
			restart_level = RESET_SUBSYS_COUPLED;
		else
			restart_level = RESET_SUBSYS_INDEPENDENT;
	}
	else
		restart_level = RESET_SOC;
#endif

	pr_info("%s: final restart_level is set to %d, board_mfg_mode %d, kernel_SSR_flag %d\n", __func__, restart_level, board_mfg_mode(), (unsigned int)(get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM));

	if (get_radio_flag() & RADIO_FLAG_USB_UPLOAD)
		enable_ramdumps = 1;

	pr_info("%s: ramdump function is %s\n", __func__, (enable_ramdumps? "Enabled": "Disabled"));
	

	ssr_wq = alloc_workqueue("ssr_wq", 0, 0);

	if (!ssr_wq)
		panic("Couldn't allocate workqueue for subsystem restart.\n");

#ifdef CONFIG_QSC_MODEM
	crashed_modem = 0;
#endif
	ret = ssr_init_soc_restart_orders();

	return ret;
}

arch_initcall(subsys_restart_init);

MODULE_DESCRIPTION("Subsystem Restart Driver");
MODULE_LICENSE("GPL v2");
