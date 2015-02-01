/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h> /* exit */
#include <errno.h>
#include <unistd.h> /* access */

#include <Ecore.h>
#include <Eina.h>

#include <dlog.h>
#include <dynamicbox_provider.h>
#include <dynamicbox_provider_buffer.h>
#include <internal/dynamicbox.h>
#include <dynamicbox.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_service.h>
#include <com-core_packet.h>

#include "critical_log.h"
#include "debug.h"
#include "so_handler.h"
#include "dbox.h"
#include "update_monitor.h"
#include "fault.h"
#include "util.h"
#include "connection.h"
#include "conf.h"

#if defined(_ESTIMATE_PERF)
#define ESTIMATE_START(id)	DbgPrint("%s START\n", id);
#define ESTIMATE_END(id)	DbgPrint("%s END\n", id);
#else
#define ESTIMATE_START(id)
#define ESTIMATE_END(id)
#endif

#define IS_DBOX_SHOWN(itm) (!(itm)->inst->item->has_dynamicbox_script || ((itm)->inst->item->has_dynamicbox_script && (itm)->is_dbox_show))

int errno;

#define UPDATE_ITEM_DELETED	(-1)
#define UPDATE_INVOKED		(-2)
#define UPDATE_NOT_INVOKED	(0)

enum gbar_open_state {
    GBAR_IS_OPENED_BUT_NOT_MINE = -1,
    GBAR_IS_NOT_OPENED = 0,
    GBAR_IS_OPENED = 1,
};

struct item {
    Ecore_Timer *timer;
    struct instance *inst;
    int monitor_cnt;
    Ecore_Timer *monitor;
    int deleteme;
    double update_interval;
    int heavy_updating; /* Only for debugging message */
    int is_paused; /* 1 is paused, 0 is resumed */
    double sleep_at;

    unsigned int updated_in_pause;

    int is_dbox_show;
    int is_gbar_show;
    int is_dbox_updated;
    int unload_so;
    struct connection *direct_path;
};

static struct info {
    Eina_List *item_list;
    Eina_List *force_update_list;
    Eina_List *update_list;
    Eina_List *pending_list;
    Eina_List *hidden_list;
    Ecore_Timer *force_update_timer;
    Ecore_Timer *pending_timer;
    Eina_List *gbar_open_pending_list;
    Ecore_Timer *gbar_open_pending_timer;
    int paused;
    Eina_List *gbar_list;
    int secured;
    int pending_timer_freezed;
    int force_timer_freezed;
} s_info  = {
    .item_list = NULL,
    .force_update_list = NULL,
    .update_list = NULL,
    .pending_list = NULL,
    .hidden_list = NULL,
    .force_update_timer = NULL,
    .pending_timer = NULL,
    .gbar_open_pending_list = NULL,
    .gbar_open_pending_timer = NULL,
    .paused = 0,
    .gbar_list = NULL,
    .secured = 0,
    .pending_timer_freezed = 0,
    .force_timer_freezed = 0,
};

static Eina_Bool updator_cb(void *data);
static inline void update_monitor_del(const char *id, struct item *item);
static int append_force_update_list(struct item *item);
static void reset_dbox_updated_flag(struct item *item);
static int append_pending_list(struct item *item);

static void pending_timer_freeze(void)
{
    DbgPrint("Freezed Count: %d\n", s_info.pending_timer_freezed);
    if (!s_info.pending_timer) {
	return;
    }

    if (!s_info.pending_timer_freezed) {
	DbgPrint("Freeze the pending timer\n");
	ecore_timer_freeze(s_info.pending_timer);
    }

    s_info.pending_timer_freezed++;
}

static void pending_timer_thaw(void)
{
    DbgPrint("Freezed Count: %d\n", s_info.pending_timer_freezed);
    if (!s_info.pending_timer_freezed) {
	return;
    }

    if (!s_info.pending_timer) {
	s_info.pending_timer_freezed = 0;
	return;
    }

    s_info.pending_timer_freezed--;
    if (!s_info.pending_timer_freezed) {
	DbgPrint("Thaw the pending timer\n");
	ecore_timer_thaw(s_info.pending_timer);
    }
}

static void force_timer_freeze(void)
{
    DbgPrint("Freeze force timer: %d\n", s_info.force_timer_freezed);
    if (!s_info.force_update_timer) {
	return;
    }

    if (!s_info.force_timer_freezed) {
	DbgPrint("Force timer freezed\n");
	ecore_timer_freeze(s_info.force_update_timer);
    }

    s_info.force_timer_freezed++;
}

static void force_timer_thaw(void)
{
    DbgPrint("Freezed force count: %d\n", s_info.force_timer_freezed);
    if (!s_info.force_timer_freezed) {
	return;
    }

    if (!s_info.force_update_timer) {
	s_info.force_timer_freezed = 0;
	return;
    }

    s_info.force_timer_freezed--;
    if (!s_info.force_timer_freezed) {
	DbgPrint("Thaw the force timer\n");
	ecore_timer_thaw(s_info.force_update_timer);
    }
}

/*
 * -1 : GBAR is opened, but not mine
 *  0 : GBAR is not opened
 *  1 : my GBAR is opened
 */
static inline enum gbar_open_state gbar_is_opened(const char *pkgname)
{
    int i;
    Eina_List *l;
    struct instance *inst;

    i = 0;
    EINA_LIST_FOREACH(s_info.gbar_list, l, inst) {
	if (pkgname && !strcmp(pkgname, inst->item->pkgname)) {
	    return GBAR_IS_OPENED;
	}

	i++;
    }

    return i > 0 ? GBAR_IS_OPENED_BUT_NOT_MINE : GBAR_IS_NOT_OPENED;
}

static Eina_Bool gbar_open_pended_cmd_consumer_cb(void *data)
{
    struct item *item;

    item = eina_list_nth(s_info.gbar_open_pending_list, 0);
    if (!item) {
	goto cleanout;
    }

    if (eina_list_data_find(s_info.update_list, item)) {
	return ECORE_CALLBACK_RENEW;
    }

    s_info.gbar_open_pending_list = eina_list_remove(s_info.gbar_open_pending_list, item);
    /*!
     * \note
     * To prevent from checking the is_updated function
     */
    if (updator_cb(item) == ECORE_CALLBACK_CANCEL) {
	/* Item is destroyed */
    }

    if (s_info.gbar_open_pending_list) {
	return ECORE_CALLBACK_RENEW;
    }

cleanout:
    s_info.gbar_open_pending_timer = NULL;
    return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool update_timeout_cb(void *data)
{
    struct item *item;

    item = data;

    ErrPrint("UPDATE TIMEOUT ========> %s - %s\n", item->inst->item->pkgname, item->inst->id);

    if (!eina_list_data_find(s_info.update_list, item)) {
	ErrPrint("Updating item is not matched\n");
    }

    fault_unmark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM);
    fault_mark_call(item->inst->item->pkgname, item->inst->id, "update,timeout", NO_ALARM, DEFAULT_LIFE_TIMER);
    s_info.update_list = eina_list_remove(s_info.update_list, item);

    exit(ETIME);
    return ECORE_CALLBACK_CANCEL;
}

static inline void update_monitor_cnt(struct item *item)
{
    double now;
    double interval;

    now = util_timestamp();
    interval = now - item->update_interval;

    /*!
     * \note
     * If the content update is processed in too short time,
     * don't increase the monitor counter, instead of it
     * set the heavy updating flag.
     * And handling this heavy updating from the
     * file update callback.
     */
    if (interval >= MINIMUM_UPDATE_INTERVAL) {
	if (eina_list_data_find(s_info.update_list, item)) {
	    /*!
	     * \note
	     * If already in updating mode,
	     * reset the monitor_cnt to 1,
	     * all updated event will be merged into A inotify event
	     */
	    DbgPrint("While waiting updated event, content is updated [%s]\n", item->inst->id);
	    item->monitor_cnt = 1;
	} else {
	    item->monitor_cnt++;
	}
    } else {
	item->heavy_updating = 1;
    }

    item->update_interval = now;
}

static inline void do_force_update(struct item *item)
{
    int ret;

    if (item->monitor) { /*!< If this item is already in update process */
	return;
    }

    if (!IS_DBOX_SHOWN(item)) {
	DbgPrint("%s is not shown yet. it will be added to normal pending list\n", item->inst->item->pkgname);
	(void)append_force_update_list(item);
	return;
    }

    if (item->is_paused) {
	DbgPrint("Item is paused. but it will be updated forcely(%s)\n", item->inst->item->pkgname);
    }

    item->updated_in_pause = 0;

    ret = so_is_updated(item->inst);
    if (ret <= 0) {
	if (so_need_to_destroy(item->inst) == DBOX_NEED_TO_DESTROY) {
	    dynamicbox_provider_send_deleted(item->inst->item->pkgname, item->inst->id);
	    dbox_destroy(item->inst->item->pkgname, item->inst->id, DBOX_DESTROY_TYPE_DEFAULT);
	    /*!
	     * \CRITICAL
	     * Every caller of this, must not access the item from now.
	     */
	    return;
	}

	reset_dbox_updated_flag(item);
	return;
    }

    /*!
     * \note
     * Check the update_list, if you want make serialized update
     */
    if (/*s_info.update_list || */gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED_BUT_NOT_MINE) {
	DbgPrint("%s is busy, migrate to normal pending list\n", item->inst->id);
	(void)append_pending_list(item);
	return;
    }

    item->monitor = ecore_timer_add(item->inst->item->timeout, update_timeout_cb, item);
    if (!item->monitor) {
	ErrPrint("Failed to add update monitor %s(%s):%d\n",
		item->inst->item->pkgname, item->inst->id, item->inst->item->timeout);
	return;
    }

    ret = so_update(item->inst);
    if (ret < 0) {
	ecore_timer_del(item->monitor);
	item->monitor = NULL;
	reset_dbox_updated_flag(item);
	return;
    }

    /*!
     * \note
     * Counter of the event monitor is only used for asynchronous content updating,
     * So reset it to 1 from here because the async updating is started now,
     * even if it is accumulated by other event function before this.
     */
    item->monitor_cnt = 1;

    /*!
     * \note
     * While waiting the Callback function call,
     * Add this for finding the crash
     */
    fault_mark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM, DEFAULT_LIFE_TIMER);

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", item->inst->item->pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	/*!
	 * \NOTE 
	 * In this case, there is potential issue
	 * 1. User added update CALLBACK -> Inotify event (Only once)
	 *    > We have to detect this case. Is it possible to be a user callback called faster than inotify event handler?
	 * 2. Inotify event -> User added update CALLBACK -> Inotify event
	 *    > Okay. What we want is this.
	 */
	update_monitor_cnt(item);
    }

    /*
     * \NOTE
     * This should be updated after "update_monitor_cnt" function call,
     * because the update_monitor_cnt function will see the s_info.update variable,
     */
    s_info.update_list = eina_list_append(s_info.update_list, item);

    return;
}

static Eina_Bool force_update_cb(void *data)
{
    struct item *item;

    item = eina_list_nth(s_info.force_update_list, 0);
    if (!item) {
	goto cleanout;
    }

    s_info.force_update_list = eina_list_remove(s_info.force_update_list, item);

    do_force_update(item);

    if (s_info.force_update_list) {
	return ECORE_CALLBACK_RENEW;
    }

cleanout:
    s_info.force_update_timer = NULL;
    return ECORE_CALLBACK_CANCEL;
}

static inline __attribute__((always_inline)) int activate_force_update_consumer(void)
{
    if (s_info.force_update_timer) {
	return DBOX_STATUS_ERROR_NONE;
    }

    s_info.force_update_timer = ecore_timer_add(0.000001f, force_update_cb, NULL);
    if (!s_info.force_update_timer) {
	ErrPrint("Failed to add a new force update timer\n");
	return DBOX_STATUS_ERROR_FAULT;
    }

    DbgPrint("Force update timer is registered\n");

    return DBOX_STATUS_ERROR_NONE;
}

static inline void deactivate_force_update_consumer(void)
{
    if (!s_info.force_update_timer) {
	return;
    }

    ecore_timer_del(s_info.force_update_timer);
    s_info.force_update_timer = NULL;

    DbgPrint("Force update timer is deleted\n");
}

static Eina_Bool pended_cmd_consumer_cb(void *data)
{
    struct item *item;

    item = eina_list_nth(s_info.pending_list, 0);
    if (!item) {
	goto cleanout;
    }

    if (eina_list_data_find(s_info.update_list, item) || gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED_BUT_NOT_MINE) {
	return ECORE_CALLBACK_RENEW;
    }

    s_info.pending_list = eina_list_remove(s_info.pending_list, item);
    /*!
     * \note
     * To prevent from checking the is_updated function
     */
    if (updator_cb(item) == ECORE_CALLBACK_CANCEL) {
	/* item is destroyed */
    }

    if (s_info.pending_list) {
	return ECORE_CALLBACK_RENEW;
    }

cleanout:
    s_info.pending_timer = NULL;
    s_info.pending_timer_freezed = 0;
    return ECORE_CALLBACK_CANCEL;
}

static inline __attribute__((always_inline)) int activate_pending_consumer(void)
{
    if (s_info.pending_timer) {
	return DBOX_STATUS_ERROR_NONE;
    }

    s_info.pending_timer = ecore_timer_add(0.000001f, pended_cmd_consumer_cb, NULL);
    if (!s_info.pending_timer) {
	ErrPrint("Failed to add a new pended command consumer\n");
	return DBOX_STATUS_ERROR_FAULT;
    }

    /*!
     * Do not increase the freezed counter.
     * Just freeze the timer.
     */
    if (s_info.pending_timer_freezed) {
	ecore_timer_freeze(s_info.pending_timer);
    }

    return DBOX_STATUS_ERROR_NONE;
}

static inline void deactivate_pending_consumer(void)
{
    if (!s_info.pending_timer) {
	return;
    }

    ecore_timer_del(s_info.pending_timer);
    s_info.pending_timer = NULL;
    s_info.pending_timer_freezed = 0;
}

static inline void deactivate_gbar_open_pending_consumer(void)
{
    if (!s_info.gbar_open_pending_timer) {
	return;
    }

    ecore_timer_del(s_info.gbar_open_pending_timer);
    s_info.gbar_open_pending_timer = NULL;
}

static inline int __attribute__((always_inline)) activate_gbar_open_pending_consumer(void)
{
    if (s_info.gbar_open_pending_timer) {
	return 0;
    }

    s_info.gbar_open_pending_timer = ecore_timer_add(0.000001f, gbar_open_pended_cmd_consumer_cb, NULL);
    if (!s_info.gbar_open_pending_timer) {
	ErrPrint("Failed to add a new pended command consumer\n");
	return DBOX_STATUS_ERROR_FAULT;
    }

    return 0;
}

static inline void migrate_to_gbar_open_pending_list(const char *pkgname)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;
    int cnt = 0;

    EINA_LIST_FOREACH_SAFE(s_info.pending_list, l, n, item) {
	if (strcmp(pkgname, item->inst->item->pkgname)) {
	    continue;
	}

	s_info.pending_list = eina_list_remove(s_info.pending_list, item);
	s_info.gbar_open_pending_list = eina_list_append(s_info.gbar_open_pending_list, item);
	cnt++;
    }

    /*!
     * These items will be moved to the pending list after the GBAR is closed.
     * Force list -> pd open list -> pending list.
     * So there is no way to go back to the foce update list again.
     *
     * \warning
     * the ITEM must only exists in one list, pending list or force_update_list
     * It is not accepted to exists in two list at same time.
     */
    EINA_LIST_FOREACH_SAFE(s_info.force_update_list, l, n, item) {
	if (strcmp(pkgname, item->inst->item->pkgname)) {
	    continue;
	}

	s_info.force_update_list = eina_list_remove(s_info.force_update_list, item);
	s_info.gbar_open_pending_list = eina_list_append(s_info.gbar_open_pending_list, item);
	cnt++;
    }

    if (s_info.gbar_open_pending_list) {
	activate_gbar_open_pending_consumer();
    }

    if (!s_info.pending_list) {
	deactivate_pending_consumer();
    }

    if (!s_info.force_update_list) {
	deactivate_force_update_consumer();
    }
}

static inline void migrate_to_pending_list(const char *pkgname)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;
    int cnt = 0;

    EINA_LIST_FOREACH_SAFE(s_info.gbar_open_pending_list, l, n, item) {
	if (strcmp(pkgname, item->inst->item->pkgname)) {
	    continue;
	}

	s_info.gbar_open_pending_list = eina_list_remove(s_info.gbar_open_pending_list, item);
	s_info.pending_list = eina_list_append(s_info.pending_list, item);
	cnt++;
    }

    if (s_info.pending_list) {
	activate_pending_consumer();
    }

    if (!s_info.gbar_open_pending_list) {
	deactivate_gbar_open_pending_consumer();
    }
}

static inline int is_pended_item(struct item *item)
{
    struct item *in_item;
    if (gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED) {
	in_item = eina_list_data_find(s_info.gbar_open_pending_list, item);
    } else {
	in_item = eina_list_data_find(s_info.pending_list, item);
    }

    return (in_item == item);
}

static int append_pending_list(struct item *item)
{
    if (item->deleteme) {
	DbgPrint("Item will be deleted, ignore update request: %s\n", item->inst->id);
	return DBOX_STATUS_ERROR_BUSY;
    }

    if (eina_list_data_find(s_info.force_update_list, item)) {
	DbgPrint("Already exists in force list\n");
	return DBOX_STATUS_ERROR_NONE;
    }

    if (gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED) {
	if (eina_list_data_find(s_info.gbar_open_pending_list, item) == item) {
	    DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
	    return DBOX_STATUS_ERROR_EXIST;
	}

	if (activate_gbar_open_pending_consumer() < 0) {
	    ErrPrint("Failed to activate GBAR open pending consumer\n");
	    return DBOX_STATUS_ERROR_FAULT;
	}

	s_info.gbar_open_pending_list = eina_list_append(s_info.gbar_open_pending_list, item);
    } else {
	if (eina_list_data_find(s_info.pending_list, item) == item) {
	    DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
	    return DBOX_STATUS_ERROR_EXIST;
	}

	if (IS_DBOX_SHOWN(item)) {
	    if (activate_pending_consumer() < 0) {
		return DBOX_STATUS_ERROR_FAULT;
	    }

	    s_info.pending_list = eina_list_append(s_info.pending_list, item);
	} else {
	    if (eina_list_data_find(s_info.hidden_list, item) == item) {
		DbgPrint("Already in hidden list - %s\n", item->inst->item->pkgname);
		return DBOX_STATUS_ERROR_EXIST;
	    }

	    s_info.hidden_list = eina_list_append(s_info.hidden_list, item);
	}
    }

    return DBOX_STATUS_ERROR_NONE;
}

static inline int clear_from_pending_list(struct item *item)
{
    Eina_List *l;
    struct item *tmp;

    EINA_LIST_FOREACH(s_info.pending_list, l, tmp) {
	if (tmp != item) {
	    continue;
	}

	s_info.pending_list = eina_list_remove_list(s_info.pending_list, l);
	if (!s_info.pending_list) {
	    deactivate_pending_consumer();
	}
	return DBOX_STATUS_ERROR_NONE;
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

static int append_force_update_list(struct item *item)
{
    if (item->deleteme) {
	DbgPrint("Item will be deleted, ignore force update request: %s\n", item->inst->id);
	return DBOX_STATUS_ERROR_BUSY;
    }

    /*!
     * If the item is already in pending list, remove it.
     */
    clear_from_pending_list(item);

    if (gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED) {
	if (eina_list_data_find(s_info.gbar_open_pending_list, item) == item) {
	    DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
	    return DBOX_STATUS_ERROR_EXIST;
	}

	if (activate_gbar_open_pending_consumer() < 0) {
	    ErrPrint("Failed to activate GBAR open pending consumer\n");
	    return DBOX_STATUS_ERROR_FAULT;
	}

	s_info.gbar_open_pending_list = eina_list_append(s_info.gbar_open_pending_list, item);
    } else {
	if (eina_list_data_find(s_info.force_update_list, item)) {
	    DbgPrint("Already in force update list\n");
	    return DBOX_STATUS_ERROR_NONE;
	}

	if (IS_DBOX_SHOWN(item)) {
	    if (activate_force_update_consumer() < 0) {
		return DBOX_STATUS_ERROR_FAULT;
	    }

	    s_info.force_update_list = eina_list_append(s_info.force_update_list, item);
	} else {
	    if (eina_list_data_find(s_info.hidden_list, item) == item) {
		DbgPrint("Already in hidden list - %s\n", item->inst->id);
		return DBOX_STATUS_ERROR_EXIST;
	    }

	    s_info.hidden_list = eina_list_append(s_info.hidden_list, item);

	    DbgPrint("forced item is moved to hidden_list - %s\n", item->inst->id);
	}
    }

    return DBOX_STATUS_ERROR_NONE;
}

static inline int clear_from_force_update_list(struct item *item)
{
    Eina_List *l;
    struct item *tmp;

    EINA_LIST_FOREACH(s_info.force_update_list, l, tmp) {
	if (tmp != item) {
	    continue;
	}

	s_info.force_update_list = eina_list_remove_list(s_info.force_update_list, l);
	if (!s_info.force_update_list) {
	    deactivate_force_update_consumer();
	}

	return DBOX_STATUS_ERROR_NONE;
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

static inline void migrate_to_pending_list_from_hidden_list(struct item *item)
{
    if (!eina_list_data_find(s_info.hidden_list, item)) {
	return;
    }

    s_info.hidden_list = eina_list_remove(s_info.hidden_list, item);
    append_pending_list(item);
}

/*!
 * \brief
 *   This function can call the update callback
 * return 0 if there is no changes
 * return -1 the item is deleted
 */
static inline int timer_thaw(struct item *item)
{
    double pending;
    double period;
    double delay;
    double sleep_time;

    if (!item->timer) {
	return 0;
    }

    ecore_timer_thaw(item->timer);
    period = ecore_timer_interval_get(item->timer);
    pending = ecore_timer_pending_get(item->timer);
    delay = util_time_delay_for_compensation(period) - pending;
    ecore_timer_delay(item->timer, delay);

    if (item->sleep_at == 0.0f) {
	return 0;
    }

    sleep_time = util_timestamp() - item->sleep_at;
    item->sleep_at = 0.0f;

    if (sleep_time > pending) {

	/*!
	 * Before do updating forcely, clear it from the pending list.
	 * We will consume it from here now.
	 */
	(void)clear_from_pending_list(item);

	if (updator_cb(item) == ECORE_CALLBACK_CANCEL) {
	    /* item is destroyed */
	    return UPDATE_ITEM_DELETED;
	} else {
	    return UPDATE_INVOKED;
	}
    }

    return UPDATE_NOT_INVOKED;
}

static void timer_freeze(struct item *item)
{
    if (!item->timer) {
	return;
    }

    ecore_timer_freeze(item->timer);

    if (ecore_timer_interval_get(item->timer) <= 1.0f) {
	return;
    }

#if defined(_USE_ECORE_TIME_GET)
    item->sleep_at = ecore_time_get();
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0) {
	ErrPrint("gettimeofday: %s\n", strerror(errno));
	tv.tv_sec = 0;
	tv.tv_usec = 0;
    }

    item->sleep_at = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
#endif
}

static inline Eina_List *find_item(struct instance *inst)
{
    Eina_List *l;
    struct item *item;

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (item->inst == inst) {
	    return l;
	}
    }

    return NULL;
}

static int desc_updated_cb(const char *filename, void *data, int over)
{
    struct item *item;

    if (over) {
	WarnPrint("Event Q overflow\n");
    }

    item = data;

    DbgPrint("DESC %s is updated\n", filename);
    if (item->is_gbar_show) {
	dynamicbox_damage_region_t region = {
	    .x = 0,
	    .y = 0,
	    .w = item->inst->w,
	    .h = item->inst->h,
	};

	dynamicbox_provider_send_updated(item->inst->item->pkgname, item->inst->id, DBOX_PRIMARY_BUFFER, &region, 1, filename);
    } else {
	ErrPrint("But GBAR is not opened, Ignore this update (%s)\n", item->inst->id);
    }
    return EXIT_SUCCESS;
}

static inline int output_handler(struct item *item)
{
    int invalid = 0;

    item->monitor_cnt--;
    if (item->monitor_cnt < 0 || item->heavy_updating) {
	if (!item->heavy_updating) {
	    WarnPrint("%s has invalid monitor_cnt\n", item->inst->id);
	    invalid = 1;
	} else {
	    item->heavy_updating = 0;	/* Reset flag */
	}

	item->monitor_cnt = 0;
    }

    if (item->monitor_cnt == 0) {
	if (!invalid) {
	    fault_unmark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM);
	}

	if (item->monitor) {
	    ecore_timer_del(item->monitor);
	    item->monitor = NULL;
	}

	s_info.update_list = eina_list_remove(s_info.update_list, item);

	if (item->deleteme) {
	    update_monitor_del(item->inst->id, item);
	    dynamicbox_provider_send_deleted(item->inst->item->pkgname, item->inst->id);
	    (void)so_destroy(item->inst, item->unload_so);
	    free(item);
	    return EXIT_FAILURE;
	}
    }

    return EXIT_SUCCESS;
}

static int file_updated_cb(const char *filename, void *data, int over)
{
    struct item *item;
    int w;
    int h;
    double priority;
    char *content = NULL;
    char *title = NULL;
    int ret;
    char *icon = NULL;
    char *name = NULL;

    if (over) {
	WarnPrint("Event Q overflow\n");
    }

    item = data;

    if (item->deleteme) {
	DbgPrint("Item is in deleting process. (%s)\n", filename);
	goto out;
    }

    ESTIMATE_START(item->inst->id);

    ret = util_get_filesize(filename);
    if (ret <= 0) {
	ErrPrint("Content is updated. but invalid. ret = %d (Update is ignored)\n", ret);
	ESTIMATE_END(item->inst->id);
	return EXIT_SUCCESS; /*!< To keep the callback */
    }

    ret = so_get_output_info(item->inst, &w, &h, &priority, &content, &title);
    if (ret < 0) {
	ErrPrint("dynamicbox_get_info returns %d\n", ret);
	ESTIMATE_END(item->inst->id);
	return EXIT_SUCCESS; /*!< To keep the callback */
    }

    /**
     * Alternative information can be NOT_IMPLEMENTED
     * And we can ignore its error.
     */
    ret = so_get_alt_info(item->inst, &icon, &name);
    if (ret < 0) {
	ErrPrint("dynamicbox_get_alt_info returns %d (ignored)\n", ret);
    }

    if (IS_DBOX_SHOWN(item)) {
	dynamicbox_damage_region_t region = {
	    .x = 0,
	    .y = 0,
	    .w = item->inst->w,
	    .h = item->inst->h,
	};
	/**
	 * If the content is not written on shared buffer (pixmap, shm, raw file, ...)
	 * We cannot use the direct path for sending updated event.
	 */
	dynamicbox_provider_send_extra_info(item->inst->item->pkgname, item->inst->id, item->inst->priority,
		content, title, item->inst->icon, item->inst->name);
	dynamicbox_provider_send_updated(item->inst->item->pkgname, item->inst->id, DBOX_PRIMARY_BUFFER, &region, 0, NULL);
    } else {
	item->is_dbox_updated++;
    }

    ESTIMATE_END(item->inst->id);
out:
    return output_handler(item);
}

static void reset_dbox_updated_flag(struct item *item)
{
    dynamicbox_damage_region_t region = {
	.x = 0,
	.y = 0,
	.w = item->inst->w,
	.h = item->inst->h,
    };

    if (!item->is_dbox_updated) {
	return;
    }

    DbgPrint("[%s] Updated %d times, (content: %s), (title: %s)\n",
	    item->inst->id, item->is_dbox_updated,
	    item->inst->content, item->inst->title);

    /**
     * If the content is not written on shared buffer (pixmap, shm, raw file, ...)
     * We cannot use the direct path for sending updated event.
     */
    dynamicbox_provider_send_extra_info(item->inst->item->pkgname, item->inst->id, item->inst->priority,
	    item->inst->content, item->inst->title, item->inst->icon, item->inst->name);
    dynamicbox_provider_send_updated(item->inst->item->pkgname, item->inst->id, DBOX_PRIMARY_BUFFER, &region, 0, NULL);

    item->is_dbox_updated = 0;
}

static inline int clear_from_gbar_open_pending_list(struct item *item)
{
    Eina_List *l;
    struct item *tmp;

    EINA_LIST_FOREACH(s_info.gbar_open_pending_list, l, tmp) {
	if (tmp != item) {
	    continue;
	}

	s_info.gbar_open_pending_list = eina_list_remove_list(s_info.gbar_open_pending_list, l);
	if (!s_info.gbar_open_pending_list) {
	    deactivate_gbar_open_pending_consumer();
	}
	return DBOX_STATUS_ERROR_NONE;
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

/*!
 * \note
 * This must has to return ECORE_CALLBACK_CANCEL, only if the item is deleted.
 * So every caller, should manage the deleted item correctly.
 */
static Eina_Bool updator_cb(void *data)
{
    struct item *item;
    int ret;

    item = data;

    if (item->monitor) { /*!< If this item is already in update process */
	return ECORE_CALLBACK_RENEW;
    }

    if (!IS_DBOX_SHOWN(item)) {
	DbgPrint("%s is not shown yet. make delay for updates\n", item->inst->item->pkgname);
	(void)append_pending_list(item);
	return ECORE_CALLBACK_RENEW;
    }

    if (item->is_paused) {
	item->updated_in_pause++;
	DbgPrint("%s is paused[%d]. make delay for updating\n", item->inst->item->pkgname, item->updated_in_pause);
	return ECORE_CALLBACK_RENEW;
    }

    item->updated_in_pause = 0;

    ret = so_is_updated(item->inst);
    if (ret <= 0) {
	if (so_need_to_destroy(item->inst) == DBOX_NEED_TO_DESTROY) {
	    dynamicbox_provider_send_deleted(item->inst->item->pkgname, item->inst->id);
	    dbox_destroy(item->inst->item->pkgname, item->inst->id, DBOX_DESTROY_TYPE_DEFAULT);
	    /*!
	     * \CRITICAL
	     * Every caller of this, must not access the item from now.
	     */
	    return ECORE_CALLBACK_CANCEL;
	}

	reset_dbox_updated_flag(item);
	return ECORE_CALLBACK_RENEW;
    }

    /*!
     * \note
     * Check the update_list, if you want make serialized update
     */
    if (/*s_info.update_list || */gbar_is_opened(item->inst->item->pkgname) == GBAR_IS_OPENED_BUT_NOT_MINE) {
	DbgPrint("%s is busy\n", item->inst->id);
	(void)append_pending_list(item);
	return ECORE_CALLBACK_RENEW;
    }

    item->monitor = ecore_timer_add(item->inst->item->timeout, update_timeout_cb, item);
    if (!item->monitor) {
	ErrPrint("Failed to add update monitor %s(%s):%d\n",
		item->inst->item->pkgname, item->inst->id, item->inst->item->timeout);
	return ECORE_CALLBACK_RENEW;
    }

    ret = so_update(item->inst);
    if (ret < 0) {
	ecore_timer_del(item->monitor);
	item->monitor = NULL;
	reset_dbox_updated_flag(item);
	return ECORE_CALLBACK_RENEW;
    }

    /*!
     * \note
     * Counter of the event monitor is only used for asynchronous content updating,
     * So reset it to 1 from here because the async updating is started now,
     * even if it is accumulated by other event function before this.
     */
    item->monitor_cnt = 1;

    /*!
     * \note
     * While waiting the Callback function call,
     * Add this for finding the crash
     */
    fault_mark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM, DEFAULT_LIFE_TIMER);

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", item->inst->item->pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	/*!
	 * \NOTE 
	 * In this case, there is potential issue
	 * 1. User added update CALLBACK -> Inotify event (Only once)
	 *    > We have to detect this case. Is it possible to be a user callback called faster than inotify event handler?
	 * 2. Inotify event -> User added update CALLBACK -> Inotify event
	 *    > Okay. What we want is this.
	 */
	update_monitor_cnt(item);
    }

    /*
     * \NOTE
     * This should be updated after "update_monitor_cnt" function call,
     * because the update_monitor_cnt function will see the s_info.update variable,
     */
    s_info.update_list = eina_list_append(s_info.update_list, item);

    return ECORE_CALLBACK_RENEW;
}

static inline void update_monitor_del(const char *id, struct item *item)
{
    char *tmp;
    int len;

    update_monitor_del_update_cb(util_uri_to_path(id), file_updated_cb);

    len = strlen(util_uri_to_path(id)) + strlen(".desc") + 1;
    tmp = malloc(len);
    if (!tmp) {
	ErrPrint("Heap: %s (%s.desc)\n", strerror(errno), util_uri_to_path(id));
	return;
    }

    snprintf(tmp, len, "%s.desc", util_uri_to_path(id));
    update_monitor_del_update_cb(tmp, desc_updated_cb);
    free(tmp);
}

static inline int add_desc_update_monitor(const char *id, struct item *item)
{
    char *filename;
    int len;
    int ret;

    len = strlen(util_uri_to_path(id)) + strlen(".desc") + 1;
    filename = malloc(len);
    if (!filename) {
	ErrPrint("Heap: %s (%s.desc)\n", strerror(errno), util_uri_to_path(id));
	return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
    }

    snprintf(filename, len, "%s.desc", util_uri_to_path(id));
    ret = update_monitor_add_update_cb(filename, desc_updated_cb, item);
    free(filename);
    return ret;
}

static inline int add_file_update_monitor(const char *id, struct item *item)
{
    return update_monitor_add_update_cb(util_uri_to_path(id), file_updated_cb, item);
}

static inline int update_monitor_add(const char *id, struct item *item)
{
    /*!
     * \NOTE
     * item->inst is not available yet.
     */
    add_file_update_monitor(id, item);
    add_desc_update_monitor(id, item);
    return DBOX_STATUS_ERROR_NONE;
}

static int disconnected_cb(int handle, void *data)
{
    Eina_List *l;
    struct item *item;
    struct connection *connection;

    connection = connection_find_by_fd(handle);
    if (!connection) {
	return 0;
    }

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (item->direct_path == connection) {
	    connection_unref(item->direct_path);
	    item->direct_path = NULL;
	}
    }

    return 0;
}

HAPI void dbox_init(void)
{
    int ret;
    ret = connection_add_event_handler(CONNECTION_EVENT_TYPE_DISCONNECTED, disconnected_cb, NULL);
    if (ret < 0) {
	ErrPrint("Unable to add an event handler\n");
    }
    return;
}

HAPI void dbox_fini(void)
{
    int ret;

    ret = dbox_delete_all_deleteme();
    if (ret < 0) {
	DbgPrint("Delete all deleteme: %d\n", ret);
    }

    ret = dbox_delete_all();
    if (ret < 0) {
	DbgPrint("Delete all: %d\n", ret);
    }

    /* Just for in case of ... */
    deactivate_pending_consumer();
    deactivate_gbar_open_pending_consumer();

    eina_list_free(s_info.gbar_open_pending_list);
    s_info.gbar_open_pending_list = NULL;
    eina_list_free(s_info.pending_list);
    s_info.pending_list = NULL;

    (void)connection_del_event_handler(CONNECTION_EVENT_TYPE_DISCONNECTED, disconnected_cb);
    return;
}

/*!
 * \note
 * Exported API for each dynamicboxes.
 */
int dynamicbox_send_updated(const char *pkgname, const char *id, int idx, int x, int y, int w, int h, int gbar, const char *descfile)
{
    Eina_List *l;
    struct item *item;
    int ret = DBOX_STATUS_ERROR_NOT_EXIST;
    dynamicbox_damage_region_t region = {
	.x = x,
	.y = y,
	.w = w,
	.h = h,
    };

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (strcmp(item->inst->item->pkgname, pkgname) || strcmp(item->inst->id, id)) {
	    continue;
	}

	if (item->direct_path) {
	    ret = dynamicbox_provider_send_direct_updated(connection_handle(item->direct_path), pkgname, id, idx, &region, gbar, descfile);
	} else {
	    ret = dynamicbox_provider_send_updated(pkgname, id, idx, &region, gbar, descfile);
	}

	break;
    }

    return ret;
}

int dynamicbox_send_buffer_updated(const char *pkgname, const char *id, dynamicbox_buffer_h handle, int idx, int x, int y, int w, int h, int gbar, const char *descfile)
{
    Eina_List *l;
    struct item *item;
    int ret = DBOX_STATUS_ERROR_NOT_EXIST;
    dynamicbox_damage_region_t region = {
	.x = x,
	.y = y,
	.w = w,
	.h = h,
    };

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (strcmp(item->inst->item->pkgname, pkgname) || strcmp(item->inst->id, id)) {
	    continue;
	}

	if (item->direct_path) {
	    ret = dynamicbox_provider_send_direct_buffer_updated(connection_handle(item->direct_path), handle, idx, &region, gbar, descfile);
	} else {
	    ret = dynamicbox_provider_send_buffer_updated(handle, idx, &region, gbar, descfile);
	}

	break;
    }

    return ret;
}

const char *dynamicbox_find_pkgname(const char *filename)
{
    Eina_List *l;
    struct item *item;

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (!strcmp(item->inst->id, filename)) {
	    return item->inst->item->pkgname;
	}
    }

    return NULL;
}

int dynamicbox_update_extra_info(const char *id, const char *content, const char *title, const char *icon, const char *name)
{
    Eina_List *l;
    struct item *item;

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (!strcmp(item->inst->id, id)) {
	    if (content && strlen(content)) {
		char *_content;

		_content = strdup(content);
		if (_content) {
		    if (item->inst->content) {
			free(item->inst->content);
			item->inst->content = NULL;
		    }

		    item->inst->content = _content;
		} else {
		    ErrPrint("Heap: %s\n", strerror(errno));
		}
	    }

	    if (title && strlen(title)) {
		char *_title;

		_title = strdup(title);
		if (_title) {
		    if (item->inst->title) {
			free(item->inst->title);
			item->inst->title = NULL;
		    }

		    item->inst->title = _title;
		} else {
		    ErrPrint("Heap: %s\n", strerror(errno));
		}
	    }

	    if (icon && strlen(icon)) {
		char *_icon;

		_icon = strdup(icon);
		if (_icon) {
		    if (item->inst->icon) {
			free(item->inst->icon);
			item->inst->icon = NULL;
		    }

		    item->inst->icon = _icon;
		} else {
		    ErrPrint("Heap: %s\n", strerror(errno));
		}
	    }

	    if (name && strlen(name)) {
		char *_name;

		_name = strdup(name);
		if (_name) {
		    if (item->inst->name) {
			free(item->inst->name);
			item->inst->name = NULL;
		    }

		    item->inst->name = _name;
		} else {
		    ErrPrint("Heap: %s\n", strerror(errno));
		}
	    }

	    return DBOX_STATUS_ERROR_NONE;
	}
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

int dynamicbox_request_update_by_id(const char *filename)
{
    Eina_List *l;
    struct item *item;

    if (so_current_op() != DBOX_OP_UNKNOWN) {
	ErrPrint("Current operation: %d\n", so_current_op());
	/*!
	 * \note
	 * Some case requires to update the content of other box from dynamicbox_XXX ABI.
	 * In that case this function can be used so we have not to filter it from here.
	 * ex) Setting accessibility.
	 * Press the assistive light, turned on, need to update other instances too.
	 * Then the box will use this function from dynamicbox_clicked function.
	 */
    }

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (!strcmp(item->inst->id, filename)) {
	    return append_pending_list(item);
	}
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

int dynamicbox_trigger_update_monitor(const char *filename, int is_gbar)
{
    char *fname;
    int ret;

    if (so_current_op() != DBOX_OP_UNKNOWN) {
	ErrPrint("Current operation: %d\n", so_current_op());
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (is_gbar) {
	int len;
	len = strlen(filename) + strlen(".desc");

	fname = malloc(len + 1);
	if (!fname) {
	    ErrPrint("Heap: %s\n", strerror(errno));
	    return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	snprintf(fname, len, "%s.desc", filename);
    } else {
	fname = strdup(filename);
	if (!fname) {
	    ErrPrint("Heap: %s\n", strerror(errno));
	    return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}
    }

    if (access(fname, R_OK | W_OK) != 0) {
	ErrPrint("access: %s (%s)\n", fname, strerror(errno));
	ret = DBOX_STATUS_ERROR_IO_ERROR;
    } else {
	ret = update_monitor_trigger_update_cb(fname, 0);
    }

    free(fname);
    return ret;
}

HAPI int dbox_open_gbar(const char *pkgname, const char *id)
{
    struct instance *inst;
    struct instance *tmp;
    Eina_List *l;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance is not found\n");
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    EINA_LIST_FOREACH(s_info.gbar_list, l, tmp) {
	if (tmp == inst) {
	    return 0;
	}
    }

    if (!s_info.gbar_list) {
	pending_timer_freeze();

	/*!
	 * \note
	 * Freeze the force timer only in this case.
	 */
	force_timer_freeze();
    }

    s_info.gbar_list = eina_list_append(s_info.gbar_list, inst);

    /*!
     * Find all instances from the pending list.
     * Move them to gbar_open_pending_timer
     */
    migrate_to_gbar_open_pending_list(pkgname);
    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_close_gbar(const char *pkgname, const char *id)
{
    Eina_List *l;
    Eina_List *n;
    struct instance *tmp;
    struct instance *inst;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Insatnce is not found\n");
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    EINA_LIST_FOREACH_SAFE(s_info.gbar_list, l, n, tmp) {
	if (tmp != inst) {
	    continue;
	}

	s_info.gbar_list = eina_list_remove(s_info.gbar_list, tmp);
	if (!s_info.gbar_list) {
	    pending_timer_thaw();
	    force_timer_thaw();
	}

	/*!
	 * Move all items in gbar_open_pending_list
	 * to pending_list.
	 */
	migrate_to_pending_list(pkgname);
	return DBOX_STATUS_ERROR_NONE;
    }

    return DBOX_STATUS_ERROR_NOT_EXIST;
}

static struct method s_table[] = {
    {
	.cmd = NULL,
	.handler = NULL,
    },
};

HAPI int dbox_create(const char *pkgname, const char *id, struct dbox_create_arg *arg, int *w, int *h, double *priority, char **content, char **title)
{
    struct instance *inst;
    struct item *item;
    int ret;
    int create_ret;
    int need_to_create;

    need_to_create = 0;
    *content = NULL;
    *title = NULL;

    inst = so_find_instance(pkgname, id);
    if (inst) {
	DbgPrint("Instance is already exists [%s - %s] content[%s], cluster[%s], category[%s], abi[%s]\n", pkgname, id, arg->content, arg->cluster, arg->category, arg->abi);
	return DBOX_STATUS_ERROR_NONE;
    }

    if (!arg->skip_need_to_create) {
	ret = so_create_needed(pkgname, arg->cluster, arg->category, arg->abi);
	if (ret != DBOX_NEED_TO_CREATE) {
	    return DBOX_STATUS_ERROR_PERMISSION_DENIED;
	}

	need_to_create = 1;
    }

    item = calloc(1, sizeof(*item));
    if (!item) {
	ErrPrint("Heap: %s (%s - %s, content[%s], cluster[%s], category[%s], abi[%s])\n", strerror(errno), pkgname, id, arg->content, arg->cluster, arg->category, arg->abi);
	return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
    }

    ret = update_monitor_add(id, item);
    if (ret < 0) {
	free(item);
	return ret;
    }

    if (arg->direct_addr) {
	item->direct_path = connection_find_by_addr(arg->direct_addr);
	if (!item->direct_path) {
	    item->direct_path = connection_create(arg->direct_addr, (void *)s_table);
	    if (!item->direct_path) {
		ErrPrint("Direct update-path is not supported\n");
	    } else {
		DbgPrint("Direct update-path is created: %s\n", id);
	    }
	} else {
	    item->direct_path = connection_ref(item->direct_path);
	    if (item->direct_path) {
		DbgPrint("Direct update-path is referred: %s\n", id);
	    }
	}
    }

    create_ret = so_create(pkgname, id, arg->content, arg->timeout, arg->has_dynamicbox_script, arg->cluster, arg->category, arg->abi, &inst);
    if (create_ret < 0) {
	update_monitor_del(id, item);
	connection_unref(item->direct_path);
	item->direct_path = NULL;
	free(item);

	*w = 0;
	*h = 0;
	*priority = 0.0f;
	return create_ret;
    }

    item->inst = inst;

    if (arg->period > 0.0f && !s_info.secured) {
	item->timer = util_timer_add(arg->period, updator_cb, item);
	if (!item->timer) {
	    ErrPrint("Failed to add timer (%s - %s, content[%s], cluster[%s], category[%s], abi[%s]\n", pkgname, id, arg->content, arg->cluster, arg->category, arg->abi);
	    update_monitor_del(id, item);
	    connection_unref(item->direct_path);
	    item->direct_path = NULL;
	    (void)so_destroy(inst, item->unload_so);
	    free(item);
	    return DBOX_STATUS_ERROR_FAULT;
	}

	if (s_info.paused) {
	    timer_freeze(item);
	}
    } else {
	DbgPrint("Local update timer is disabled: %lf (%d)\n", arg->period, s_info.secured);
	item->timer = NULL;
    }

    s_info.item_list = eina_list_append(s_info.item_list, item);

    if (create_ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (create_ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_FORCE_UPDATE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (create_ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
	/*!
	 * \note
	 * To send a output info, get the info forcely.
	 * but the output file monitor will do this again
	 *
	 * This function will set the tmp_content and tmp_title
	 * even if it has no updates on the content, title,
	 * it will set them to NULL.
	 */
	if (so_get_output_info(inst, w, h, priority, content, title) == (int)DBOX_STATUS_ERROR_NONE) {
	    if (*content) {
		char *tmp;

		tmp = strdup(*content);
		if (!tmp) {
		    ErrPrint("Memory: %s\n", strerror(errno));
		}

		*content = tmp;
	    }

	    if (*title) {
		char *tmp;

		tmp = strdup(*title);
		if (!tmp) {
		    ErrPrint("Memory: %s\n", strerror(errno));
		}

		*title = tmp;
	    }
	}
    }

    *w = inst->w;
    *h = inst->h;
    *priority = inst->priority;
    return need_to_create;
}

HAPI int dbox_destroy(const char *pkgname, const char *id, int type)
{
    Eina_List *l;
    Eina_List *n;
    struct instance *inst;
    struct instance *tmp;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created\n", pkgname, id);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    EINA_LIST_FOREACH_SAFE(s_info.gbar_list, l, n, tmp) {
	if (tmp != inst) {
	    continue;
	}

	s_info.gbar_list = eina_list_remove(s_info.gbar_list, tmp);
	if (!s_info.gbar_list) {
	    pending_timer_thaw();
	    force_timer_thaw();
	}

	/*!
	 * Move all items in gbar_open_pending_list
	 * to pending_list.
	 */
	migrate_to_pending_list(pkgname);
	break;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    s_info.item_list = eina_list_remove_list(s_info.item_list, l);

    s_info.update_list = eina_list_remove(s_info.update_list, item);
    s_info.hidden_list = eina_list_remove(s_info.hidden_list, item);
    clear_from_gbar_open_pending_list(item);
    clear_from_pending_list(item);
    clear_from_force_update_list(item);
    (void)connection_unref(item->direct_path);
    item->direct_path = NULL;

    if (item->timer) {
	ecore_timer_del(item->timer);
	item->timer = NULL;
    }

    /*
     * To keep the previous status, we should or'ing the value.
     */
    item->unload_so = (item->unload_so || (type == DBOX_DESTROY_TYPE_UNINSTALL) || (type == DBOX_DESTROY_TYPE_UPGRADE));

    if (item->monitor) {
	item->deleteme = 1;
    } else {
	update_monitor_del(id, item);
	(void)so_destroy(inst, item->unload_so);
	free(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_resize(const char *pkgname, const char *id, int w, int h)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created (%dx%d)\n", pkgname, id, w, h);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s, %dx%d)\n", pkgname, id, w, h);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);

    ESTIMATE_START(id);
    ret = so_resize(inst, w, h);
    ESTIMATE_END(id);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI char *dbox_pinup(const char *pkgname, const char *id, int pinup)
{
    struct instance *inst;
    char *ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not found (pinup[%d])\n", pkgname, id, pinup);
	return NULL;
    }

    ret = so_pinup(inst, pinup);
    return ret;
}

HAPI int dbox_set_period(const char *pkgname, const char *id, double period)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not found (period[%lf])\n", pkgname, id, period);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s, period[%lf])\n", pkgname, id, period);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);

    if (period <= 0.0f) {
	if (item->timer) {
	    ecore_timer_del(item->timer);
	    item->timer = NULL;
	}
    } else {
	if (item->timer) {
	    util_timer_interval_set(item->timer, period);
	} else if (!s_info.secured) {
	    item->timer = util_timer_add(period, updator_cb, item);
	    if (!item->timer) {
		ErrPrint("Failed to add timer (%s - %s)\n", pkgname, id);
		return DBOX_STATUS_ERROR_FAULT;
	    }

	    if (s_info.paused) {
		timer_freeze(item);
	    }
	}
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_clicked(const char *pkgname, const char *id, const char *event, double timestamp, double x, double y)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not exists (event[%s])\n", pkgname, id, event);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s, event[%s])\n", pkgname, id, event);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);

    ESTIMATE_START(id);
    ret = so_clicked(inst, event, timestamp, x, y);
    ESTIMATE_END(id);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_script_event(const char *pkgname, const char *id, const char *emission, const char *source, dynamicbox_event_info_t event_info)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not exists (emission[%s], source[%s])\n", pkgname, id, emission, source);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s, emissino[%s], source[%s])\n", pkgname, id, emission, source);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);

    if (emission && source && !strcmp(source, id)) {
	if (item->inst->item->has_dynamicbox_script) {
	    if (!strcmp(emission, "dbox,show")) {
		item->is_dbox_show = 1;

		migrate_to_pending_list_from_hidden_list(item);

		if (item->is_dbox_updated && !is_pended_item(item)) {
		    reset_dbox_updated_flag(item);
		}

		source = util_uri_to_path(source);
	    } else if (!strcmp(emission, "dbox,hide")) {
		DbgPrint("Livebox(%s) script is hide now\n", id);
		item->is_dbox_show = 0;

		source = util_uri_to_path(source);
	    }
	}

	if (!strcmp(emission, "gbar,show")) {
	    item->is_gbar_show = 1;
	    source = util_uri_to_path(source);
	} else if (!strcmp(emission, "gbar,hide")) {
	    item->is_gbar_show = 0;
	    source = util_uri_to_path(source);
	}
    }

    ESTIMATE_START(id);
    ret = so_script_event(inst, emission, source, event_info);
    ESTIMATE_END(id);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_is_pinned_up(const char *pkgname, const char *id)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created\n", pkgname, id);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    if (!item) {
	ErrPrint("Invalid item(%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_FAULT;
    }
    /*!
     * NOTE:
     * item is not used.
     * Maybe this is not neccessary for this operation
     */
    return so_is_pinned_up(inst);
}

HAPI int dbox_change_group(const char *pkgname, const char *id, const char *cluster, const char *category)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created (cluster[%s], category[%s])\n", pkgname, id, cluster, category);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found(%s - %s, cluster[%s], category[%s])\n", pkgname, id, cluster, category);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);

    ret = so_change_group(inst, cluster, category);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

static int dbox_sys_event(struct instance *inst, struct item *item, int event)
{
    int ret;

    ret = so_sys_event(inst, event);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", item->inst->item->pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_system_event(const char *pkgname, const char *id, int event)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("instance %s - %s is not created\n", pkgname, id);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    return dbox_sys_event(inst, item, event);
}

HAPI int dbox_update(const char *pkgname, const char *id, int force)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created\n", pkgname, id);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    if (force && gbar_is_opened(pkgname) != GBAR_IS_OPENED) {
	(void)append_force_update_list(item);
    } else {
	(void)append_pending_list(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_set_content_info(const char *pkgname, const char *id, const char *content_info)
{
    Eina_List *l;
    struct instance *inst;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	ErrPrint("Instance %s - %s is not created (%s)\n", pkgname, id, content_info);
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s, %s)\n", pkgname, id, content_info);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    ESTIMATE_START(id);
    ret = so_set_content_info(inst, content_info);
    ESTIMATE_END(id);
    if (ret < 0) {
	return ret;
    }

    if (ret & DBOX_NEED_TO_SCHEDULE) {
	DbgPrint("%s Returns DBOX_NEED_TO_SCHEDULE\n", pkgname);
	(void)append_pending_list(item);
    }

    if (ret & DBOX_FORCE_TO_SCHEDULE) {
	DbgPrint("%s Return DBOX_NEED_TO_FORCE_SCHEDULE\n", pkgname);
	(void)append_force_update_list(item);
    }

    if (ret & DBOX_OUTPUT_UPDATED) {
	update_monitor_cnt(item);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_update_all(const char *pkgname, const char *cluster, const char *category, int force)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;

    DbgPrint("Update content for %s\n", pkgname ? pkgname : "(all)");
    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	if (item->deleteme) {
	    continue;
	}

	if (cluster && strcasecmp(item->inst->cluster, cluster)) {
	    continue;
	}

	if (category && strcasecmp(item->inst->category, category)) {
	    continue;
	}

	if (pkgname && strlen(pkgname)) {
	    if (!strcmp(item->inst->item->pkgname, pkgname)) {
		if (force && gbar_is_opened(pkgname) != GBAR_IS_OPENED) {
		    (void)append_force_update_list(item);
		} else {
		    (void)append_pending_list(item);
		}
	    }
	} else {
	    if (force) {
		DbgPrint("Update All function doesn't support force update to all dynamicboxes\n");
	    } else {
		(void)append_pending_list(item);
	    }
	}
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_set_content_info_all(const char *pkgname, const char *content)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;
    register int cnt = 0;

    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	if (item->deleteme) {
	    continue;
	}

	if (pkgname && strlen(pkgname)) {
	    if (!strcmp(item->inst->item->pkgname, pkgname)) {
		dbox_set_content_info(item->inst->item->pkgname, item->inst->id, content);
		cnt++;
	    }
	} else {
	    dbox_set_content_info(item->inst->item->pkgname, item->inst->id, content);
	    cnt++;
	}
    }
    DbgPrint("Update content for %s - %d\n", pkgname ? pkgname : "(all)", cnt);

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_delete_all_deleteme(void)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;
    int cnt = 0;

    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	if (!item->deleteme) {
	    continue;
	}

	update_monitor_del(item->inst->id, item);
	(void)so_destroy(item->inst, item->unload_so);
	free(item);
	cnt++;
    }

    DbgPrint("Delete all deleteme: %d\n", cnt);
    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_delete_all(void)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;
    int cnt = 0;

    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	update_monitor_del(item->inst->id, item);
	(void)so_destroy(item->inst, item->unload_so);
	free(item);
	cnt++;
    }

    DbgPrint("Delete all deleteme: %d\n", cnt);
    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_system_event_all(int event)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;

    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	if (item->deleteme) {
	    continue;
	}

	DbgPrint("System event for %s (%d)\n", item->inst->id, event);
	dbox_sys_event(item->inst, item, event);
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI void dbox_pause_all(void)
{
    Eina_List *l;
    struct item *item;

    s_info.paused = 1;

    pending_timer_freeze();
    /*!
     * \note
     * force timer will not be freezed
     */

    EINA_LIST_FOREACH(s_info.item_list, l, item) {
	if (item->deleteme) {
	    DbgPrint("Instance %s skip timer pause (deleteme)\n", item->inst->item->pkgname);
	    continue;
	}

	if (item->is_paused) {
	    continue;
	}

	timer_freeze(item);

	dbox_sys_event(item->inst, item, DBOX_SYS_EVENT_PAUSED);
    }
}

HAPI void dbox_resume_all(void)
{
    Eina_List *l;
    Eina_List *n;
    struct item *item;

    s_info.paused = 0;

    pending_timer_thaw();

    /*!
     * \note
     * force timer will not affected by this
     */

    EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
	if (item->deleteme) {
	    DbgPrint("Instance %s skip timer resume (deleteme)\n", item->inst->item->pkgname);
	    continue;
	}

	if (item->is_paused) {
	    continue;
	}

	dbox_sys_event(item->inst, item, DBOX_SYS_EVENT_RESUMED);

	if (item->updated_in_pause) {
	    (void)append_pending_list(item);
	    item->updated_in_pause = 0;
	}

	/*!
	 * \note
	 * After send the resume callback, call this function.
	 * Because the timer_thaw can call the update function.
	 * Before resumed event is notified to the dynamicbox,
	 * Do not call update function
	 */
	if (timer_thaw(item) == UPDATE_ITEM_DELETED) {
	    /* item is deleted */
	}
    }
}

HAPI int dbox_pause(const char *pkgname, const char *id)
{
    struct instance *inst;
    Eina_List *l;
    struct item *item;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    if (!item) {
	return DBOX_STATUS_ERROR_FAULT;
    }

    if (item->deleteme) {
	DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
	return DBOX_STATUS_ERROR_BUSY;
    }

    item->is_paused = 1;

    if (s_info.paused) {
	return DBOX_STATUS_ERROR_NONE;
    }

    timer_freeze(item);

    dbox_sys_event(inst, item, DBOX_SYS_EVENT_PAUSED);

    return DBOX_STATUS_ERROR_NONE;
}

HAPI int dbox_resume(const char *pkgname, const char *id)
{
    struct instance *inst;
    Eina_List *l;
    struct item *item;
    int ret;

    inst = so_find_instance(pkgname, id);
    if (!inst) {
	return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    l = find_item(inst);
    if (!l) {
	ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
	return DBOX_STATUS_ERROR_NOT_EXIST;
    }

    item = eina_list_data_get(l);
    if (!item) {
	return DBOX_STATUS_ERROR_FAULT;
    }

    if (item->deleteme) {
	DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
	return DBOX_STATUS_ERROR_BUSY;
    }

    item->is_paused = 0;

    if (s_info.paused) {
	return DBOX_STATUS_ERROR_NONE;
    }

    dbox_sys_event(inst, item, DBOX_SYS_EVENT_RESUMED);

    ret = timer_thaw(item);
    if (ret == UPDATE_ITEM_DELETED) {
	/*!
	 * \note
	 * ITEM is deleted
	 */
	return DBOX_STATUS_ERROR_NONE;
    } else if (ret == UPDATE_INVOKED) {
	/*!
	 * \note
	 * if the update is successfully done, the updated_in_pause will be reset'd.
	 * or append it to the pending list
	 */
    }

    if (item->updated_in_pause) {
	(void)append_pending_list(item);
	item->updated_in_pause = 0;
    }

    return DBOX_STATUS_ERROR_NONE;
}

HAPI void dbox_turn_secured_on(void)
{
    s_info.secured = 1;
}

HAPI int dbox_is_secured(void)
{
    return s_info.secured;
}

HAPI int dbox_is_all_paused(void)
{
    return s_info.paused;
}

/* End of a file */
