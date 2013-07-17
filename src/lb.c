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
#include <provider.h>
#include <livebox.h>
#include <livebox-errno.h>

#include "critical_log.h"
#include "debug.h"
#include "conf.h"
#include "so_handler.h"
#include "lb.h"
#include "update_monitor.h"
#include "fault.h"
#include "util.h"

#define IS_LB_SHOWN(itm) (!(itm)->inst->item->has_livebox_script || ((itm)->inst->item->has_livebox_script && (itm)->is_lb_show))

int errno;

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

	int is_lb_show;
	int is_pd_show;
	int is_lb_updated;
};

static struct info {
	Eina_List *item_list;
	struct item *update;
	Eina_List *pending_list;
	Ecore_Timer *pending_timer;
	Eina_List *pd_open_pending_list;
	Ecore_Timer *pd_open_pending_timer;
	int paused;
	Eina_List *pd_list;
	int secured;
	int pending_timer_freezed;
} s_info  = {
	.item_list = NULL,
	.update = NULL,
	.pending_list = NULL,
	.pending_timer = NULL,
	.pd_open_pending_list = NULL,
	.pd_open_pending_timer = NULL,
	.paused = 0,
	.pd_list = NULL,
	.secured = 0,
	.pending_timer_freezed = 0,
};

static Eina_Bool updator_cb(void *data);

static void pending_timer_freeze(void)
{
	DbgPrint("Freezed Count: %d\n", s_info.pending_timer_freezed);
	if (s_info.pending_timer && !s_info.pending_timer_freezed) {
		DbgPrint("Freeze the pending timer\n");
		ecore_timer_freeze(s_info.pending_timer);
	}

	s_info.pending_timer_freezed++;
}

static void pending_timer_thaw(void)
{
	DbgPrint("Freezed Count: %d\n", s_info.pending_timer_freezed);
	if (!s_info.pending_timer_freezed)
		return;

	s_info.pending_timer_freezed--;
	if (s_info.pending_timer && !s_info.pending_timer_freezed) {
		DbgPrint("Thaw the pending timer\n");
		ecore_timer_thaw(s_info.pending_timer);
	}
}

/*
 * -1 : PD is opened, but not mine
 *  0 : PD is not opened
 *  1 : my PD is opened
 */
static inline int pd_is_opened(const char *pkgname)
{
	int i;
	Eina_List *l;
	char *tmp;

	i = 0;
	EINA_LIST_FOREACH(s_info.pd_list, l, tmp) {
		if (pkgname && !strcmp(pkgname, tmp))
			return 1;

		i++;
	}

	return i > 0 ? -1 : 0;
}

static Eina_Bool pd_open_pended_cmd_consumer_cb(void *data)
{
	struct item *item;

	item = eina_list_nth(s_info.pd_open_pending_list, 0);
	if (!item)
		goto cleanout;

	if (s_info.update)
		return ECORE_CALLBACK_RENEW;

	s_info.pd_open_pending_list = eina_list_remove(s_info.pd_open_pending_list, item);
	/*!
	 * \note
	 * To prevent from checking the is_updated function
	 */
	(void)updator_cb(item);
	if (s_info.pd_open_pending_list)
		return ECORE_CALLBACK_RENEW;

cleanout:
	s_info.pd_open_pending_timer = NULL;
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool pended_cmd_consumer_cb(void *data)
{
	struct item *item;

	item = eina_list_nth(s_info.pending_list, 0);
	if (!item)
		goto cleanout;

	if (s_info.update || pd_is_opened(item->inst->item->pkgname) < 0)
		return ECORE_CALLBACK_RENEW;

	s_info.pending_list = eina_list_remove(s_info.pending_list, item);
	/*!
	 * \note
	 * To prevent from checking the is_updated function
	 */
	(void)updator_cb(item);
	if (s_info.pending_list)
		return ECORE_CALLBACK_RENEW;

cleanout:
	s_info.pending_timer = NULL;
	s_info.pending_timer_freezed = 0;
	return ECORE_CALLBACK_CANCEL;
}

static inline __attribute__((always_inline)) int activate_pending_consumer(void)
{
	if (s_info.pending_timer)
		return 0;

	s_info.pending_timer = ecore_timer_add(0.000001f, pended_cmd_consumer_cb, NULL);
	if (!s_info.pending_timer) {
		ErrPrint("Failed to add a new pended command consumer\n");
		return LB_STATUS_ERROR_FAULT;
	}

	/*!
	 * Do not increase the freezed counter.
	 * Just freeze the timer.
	 */
	if (s_info.pending_timer_freezed)
		ecore_timer_freeze(s_info.pending_timer);

	return 0;
}

static inline void deactivate_pending_consumer(void)
{
	if (!s_info.pending_timer)
		return;

	ecore_timer_del(s_info.pending_timer);
	s_info.pending_timer = NULL;
	s_info.pending_timer_freezed = 0;
}

static inline void deactivate_pd_open_pending_consumer(void)
{
	if (!s_info.pd_open_pending_timer)
		return;

	ecore_timer_del(s_info.pd_open_pending_timer);
	s_info.pd_open_pending_timer = NULL;
}

static inline int __attribute__((always_inline)) activate_pd_open_pending_consumer(void)
{
	if (s_info.pd_open_pending_timer)
		return 0;

	s_info.pd_open_pending_timer = ecore_timer_add(0.000001f, pd_open_pended_cmd_consumer_cb, NULL);
	if (!s_info.pd_open_pending_timer) {
		ErrPrint("Failed to add a new pended command consumer\n");
		return LB_STATUS_ERROR_FAULT;
	}

	return 0;
}

static inline void migrate_to_pd_open_pending_list(const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;
	int cnt = 0;

	EINA_LIST_FOREACH_SAFE(s_info.pending_list, l, n, item) {
		if (strcmp(pkgname, item->inst->item->pkgname))
			continue;

		s_info.pending_list = eina_list_remove(s_info.pending_list, item);
		s_info.pd_open_pending_list = eina_list_append(s_info.pd_open_pending_list, item);
		cnt++;
	}

	if (s_info.pd_open_pending_list)
		activate_pd_open_pending_consumer();

	if (!s_info.pending_list)
		deactivate_pending_consumer();
}

static inline void migrate_to_pending_list(const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;
	int cnt = 0;

	EINA_LIST_FOREACH_SAFE(s_info.pd_open_pending_list, l, n, item) {
		if (strcmp(pkgname, item->inst->item->pkgname))
			continue;

		s_info.pd_open_pending_list = eina_list_remove(s_info.pd_open_pending_list, item);
		s_info.pending_list = eina_list_append(s_info.pending_list, item);
		cnt++;
	}

	if (s_info.pending_list)
		activate_pending_consumer();

	if (!s_info.pd_open_pending_list)
		deactivate_pd_open_pending_consumer();
}

static inline int is_pended_item(struct item *item)
{
	struct item *in_item;
	if (pd_is_opened(item->inst->item->pkgname) == 1) {
		in_item = eina_list_data_find(s_info.pd_open_pending_list, item);
	} else {
		in_item = eina_list_data_find(s_info.pending_list, item);
	}

	return (in_item == item);
}

static int append_pending_list(struct item *item)
{
	if (pd_is_opened(item->inst->item->pkgname) == 1) {
		if (eina_list_data_find(s_info.pd_open_pending_list, item) == item) {
			DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
			return LB_STATUS_ERROR_EXIST;
		}

		if (activate_pd_open_pending_consumer() < 0) {
			ErrPrint("Failed to activate PD open pending consumer\n");
			return LB_STATUS_ERROR_FAULT;
		}

		s_info.pd_open_pending_list = eina_list_append(s_info.pd_open_pending_list, item);
	} else {
		if (eina_list_data_find(s_info.pending_list, item) == item) {
			DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
			return LB_STATUS_ERROR_EXIST;
		}

		if (activate_pending_consumer() < 0)
			return LB_STATUS_ERROR_FAULT;

		s_info.pending_list = eina_list_append(s_info.pending_list, item);
	}
	return 0;
}

static inline void timer_thaw(struct item *item)
{
	double pending;
	double period;
	double delay;
	double sleep_time;

	if (!item->timer)
		return;

	ecore_timer_thaw(item->timer);
	period = ecore_timer_interval_get(item->timer);
	pending = ecore_timer_pending_get(item->timer);
	delay = util_time_delay_for_compensation(period) - pending;
	ecore_timer_delay(item->timer, delay);

	if (item->sleep_at == 0.0f)
		return;

	sleep_time = util_timestamp() - item->sleep_at;
	if (sleep_time > pending)
		(void)updator_cb(item);

	item->sleep_at = 0.0f;
}

static inline void timer_freeze(struct item *item)
{
	struct timeval tv;

	if (!item->timer)
		return;

	ecore_timer_freeze(item->timer);

	if (ecore_timer_interval_get(item->timer) <= 1.0f)
		return;

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("gettimeofday: %s\n", strerror(errno));
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	item->sleep_at = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
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
		if (s_info.update == item) {
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

static inline Eina_List *find_item(struct instance *inst)
{
	Eina_List *l;
	struct item *item;
	
	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (item->inst == inst)
			return l;
	}

	return NULL;
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
		if (!invalid)
			fault_unmark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM);

		if (item->monitor) {
			ecore_timer_del(item->monitor);
			item->monitor = NULL;
		}

		if (s_info.update == item)
			s_info.update = NULL;

		if (item->deleteme) {
			provider_send_deleted(item->inst->item->pkgname, item->inst->id);
			(void)so_destroy(item->inst);
			free(item);
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

static int desc_updated_cb(const char *filename, void *data, int over)
{
	struct item *item;

	if (over)
		WarnPrint("Event Q overflow\n");

	item = data;

	DbgPrint("DESC %s is updated\n", filename);
	if (item->is_pd_show) {
		provider_send_desc_updated(item->inst->item->pkgname, item->inst->id, filename);
	} else {
		ErrPrint("But PD is not opened, Ignore this update (%s)\n", item->inst->id);
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

	if (over)
		WarnPrint("Event Q overflow\n");

	item = data;

	ret = util_get_filesize(filename);
	if (ret <= 0) {
		ErrPrint("Content is updated. but invalid. ret = %d (Update is ignored)\n", ret);
		return EXIT_SUCCESS; /*!< To keep the callback */
	}

	ret = so_get_output_info(item->inst, &w, &h, &priority, &content, &title);
	if (ret < 0) {
		ErrPrint("livebox_get_info returns %d\n", ret);
		return EXIT_SUCCESS; /*!< To keep the callback */
	}

	if (IS_LB_SHOWN(item)) {
		provider_send_updated(item->inst->item->pkgname, item->inst->id,
					item->inst->w, item->inst->h, item->inst->priority,
					content, title);
	} else {
		item->is_lb_updated++;
	}

	return output_handler(item);
}

static void reset_lb_updated_flag(struct item *item)
{
	if (!item->is_lb_updated)
		return;

	DbgPrint("[%s] Updated %d times, (content: %s), (title: %s)\n",
			item->inst->id, item->is_lb_updated,
			item->inst->content, item->inst->title);

	provider_send_updated(item->inst->item->pkgname, item->inst->id,
			item->inst->w, item->inst->h, item->inst->priority,
			item->inst->content, item->inst->title);

	item->is_lb_updated = 0;
}

static inline int clear_from_pd_open_pending_list(struct item *item)
{
	Eina_List *l;
	struct item *tmp;

	EINA_LIST_FOREACH(s_info.pd_open_pending_list, l, tmp) {
		if (tmp != item)
			continue;

		s_info.pd_open_pending_list = eina_list_remove_list(s_info.pd_open_pending_list, l);
		if (!s_info.pd_open_pending_list)
			deactivate_pd_open_pending_consumer();
		return LB_STATUS_SUCCESS;
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

static inline int clear_from_pending_list(struct item *item)
{
	Eina_List *l;
	struct item *tmp;

	EINA_LIST_FOREACH(s_info.pending_list, l, tmp) {
		if (tmp != item)
			continue;

		s_info.pending_list = eina_list_remove_list(s_info.pending_list, l);
		if (!s_info.pending_list)
			deactivate_pending_consumer();
		return LB_STATUS_SUCCESS;
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

static Eina_Bool update_timeout_cb(void *data)
{
	struct item *item;

	item = data;

	ErrPrint("UPDATE TIMEOUT ========> %s - %s\n", item->inst->item->pkgname, item->inst->id);

	if (s_info.update != item)
		ErrPrint("Updating item is not matched\n");

	fault_unmark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM);
	fault_mark_call(item->inst->item->pkgname, item->inst->id, "update,timeout", NO_ALARM, DEFAULT_LIFE_TIMER);
	s_info.update = NULL;

	exit(ETIME);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool updator_cb(void *data)
{
	struct item *item;
	int ret;

	item = data;

	if (item->monitor) {/*!< If this item is already in update process */
		return ECORE_CALLBACK_RENEW;
	}

	if (!IS_LB_SHOWN(item)) {
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
		if (so_need_to_destroy(item->inst) == NEED_TO_DESTROY) {
			provider_send_deleted(item->inst->item->pkgname, item->inst->id);
			lb_destroy(item->inst->item->pkgname, item->inst->id);
			return ECORE_CALLBACK_CANCEL;
		}

		reset_lb_updated_flag(item);
		return ECORE_CALLBACK_RENEW;
	}

	if (s_info.update || pd_is_opened(item->inst->item->pkgname) < 0) {
		DbgPrint("%s is busy\n", s_info.update ? s_info.update->inst->id : item->inst->id);
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
		reset_lb_updated_flag(item);
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

	if (ret & NEED_TO_SCHEDULE)
		(void)append_pending_list(item);

	if (ret & OUTPUT_UPDATED) {
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
	s_info.update = item;

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

	len = strlen(util_uri_to_path(id)) + strlen(".desc") + 1;
	filename = malloc(len);
	if (!filename) {
		ErrPrint("Heap: %s (%s.desc)\n", strerror(errno), util_uri_to_path(id));
		return LB_STATUS_ERROR_MEMORY;
	}

	snprintf(filename, len, "%s.desc", util_uri_to_path(id));
	return update_monitor_add_update_cb(filename, desc_updated_cb, item);
}

static inline int add_file_update_monitor(const char *id, struct item *item)
{
	char *filename;

	filename = strdup(util_uri_to_path(id));
	if (!filename) {
		ErrPrint("Heap: %s (%s)\n", strerror(errno), id);
		return LB_STATUS_ERROR_MEMORY;
	}

	return update_monitor_add_update_cb(filename, file_updated_cb, item);
}

static inline int update_monitor_add(const char *id, struct item *item)
{
	/*!
	 * \NOTE
	 * item->inst is not available yet.
	 */
	add_file_update_monitor(id, item);
	add_desc_update_monitor(id, item);
	return LB_STATUS_SUCCESS;
}

HAPI int lb_init(void)
{
	return LB_STATUS_SUCCESS;
}

HAPI int lb_fini(void)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;

	deactivate_pending_consumer();
	deactivate_pd_open_pending_consumer();

	EINA_LIST_FREE(s_info.pd_open_pending_list, item);
	EINA_LIST_FREE(s_info.pending_list, item);

	EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
		provider_send_deleted(item->inst->item->pkgname, item->inst->id);
		lb_destroy(item->inst->item->pkgname, item->inst->id);
	}

	return LB_STATUS_SUCCESS;
}

/*!
 * \note
 * Exported API for each liveboxes.
 */
const char *livebox_find_pkgname(const char *filename)
{
	Eina_List *l;
	struct item *item;

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (!strcmp(item->inst->id, filename))
			return item->inst->item->pkgname;
	}

	return NULL;
}

int livebox_request_update_by_id(const char *filename)
{
	Eina_List *l;
	struct item *item;

	if (so_current_op() != LIVEBOX_OP_UNKNOWN) {
		ErrPrint("Current operation: %d\n", so_current_op());
		return LB_STATUS_ERROR_INVALID;
	}

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (!strcmp(item->inst->id, filename)) {
			return append_pending_list(item);
		}
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

int livebox_trigger_update_monitor(const char *filename, int is_pd)
{
	char *fname;
	int ret;

	if (so_current_op() != LIVEBOX_OP_UNKNOWN) {
		ErrPrint("Current operation: %d\n", so_current_op());
		return LB_STATUS_ERROR_INVALID;
	}

	if (is_pd) {
		int len;
		len = strlen(filename) + strlen(".desc");

		fname = malloc(len + 1);
		if (!fname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return LB_STATUS_ERROR_MEMORY;
		}

		snprintf(fname, len, "%s.desc", filename);
	} else {
		fname = strdup(filename);
		if (!fname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return LB_STATUS_ERROR_MEMORY;
		}
	}

	if (access(fname, R_OK | W_OK) != 0) {
		ErrPrint("access: %s (%s)\n", fname, strerror(errno));
		ret = LB_STATUS_ERROR_IO;
	} else {
		ret = update_monitor_trigger_update_cb(fname, 0);
	}

	free(fname);
	return ret;
}

HAPI int lb_open_pd(const char *pkgname)
{
	Eina_List *l;
	char *tmp;

	EINA_LIST_FOREACH(s_info.pd_list, l, tmp) {
		if (!strcmp(pkgname, tmp))
			return 0;
	}

	tmp = strdup(pkgname);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	if (!s_info.pd_list)
		pending_timer_freeze();

	s_info.pd_list = eina_list_append(s_info.pd_list, tmp);

	/*!
	 * Find all instances from the pending list.
	 * Move them to pd_open_pending_timer
	 */
	migrate_to_pd_open_pending_list(pkgname);
	return LB_STATUS_SUCCESS;
}

HAPI int lb_close_pd(const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	char *tmp;

	EINA_LIST_FOREACH_SAFE(s_info.pd_list, l, n, tmp) {
		if (strcmp(tmp, pkgname))
			continue;

		s_info.pd_list = eina_list_remove(s_info.pd_list, tmp);
		free(tmp);

		if (!s_info.pd_list)
			pending_timer_thaw();

		/*!
		 * Move all items in pd_open_pending_list
		 * to pending_list.
		 */
		migrate_to_pending_list(pkgname);
		return LB_STATUS_SUCCESS;
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI int lb_create(const char *pkgname, const char *id, const char *content_info, int timeout, int has_livebox_script, double period, const char *cluster, const char *category, int *w, int *h, double *priority, int skip_need_to_create, const char *abi, char **out_content, char **out_title)
{
	struct instance *inst;
	struct item *item;
	int ret;
	int create_ret;
	int need_to_create;

	need_to_create = 0;
	*out_content = NULL;
	*out_title = NULL;

	inst = so_find_instance(pkgname, id);
	if (inst) {
		DbgPrint("Instance is already exists [%s - %s] content[%s], cluster[%s], category[%s], abi[%s]\n", pkgname, id, content_info, cluster, category, abi);
		return LB_STATUS_SUCCESS;
	}

	if (!skip_need_to_create) {
		ret = so_create_needed(pkgname, cluster, category, abi);
		if (ret != NEED_TO_CREATE)
			return LB_STATUS_ERROR_PERMISSION;

		need_to_create = 1;
	}

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s (%s - %s, content[%s], cluster[%s], category[%s], abi[%s])\n", strerror(errno), pkgname, id, content_info, cluster, category, abi);
		return LB_STATUS_ERROR_MEMORY;
	}

	ret = update_monitor_add(id, item);
	if (ret < 0) {
		free(item);
		return ret;
	}

	create_ret = so_create(pkgname, id, content_info, timeout, has_livebox_script, cluster, category, abi, &inst);
	if (create_ret < 0) {
		update_monitor_del(id,  item);
		free(item);

		*w = 0;
		*h = 0;
		*priority = 0.0f;
		return create_ret;
	}

	item->inst = inst;

	if (period > 0.0f && !s_info.secured) {
		item->timer = util_timer_add(period, updator_cb, item);
		if (!item->timer) {
			ErrPrint("Failed to add timer (%s - %s, content[%s], cluster[%s], category[%s], abi[%s]\n", pkgname, id, content_info, cluster, category, abi);
			update_monitor_del(id, item);
			(void)so_destroy(inst);
			free(item);
			return LB_STATUS_ERROR_FAULT;
		}

		if (s_info.paused)
			timer_freeze(item);
	} else {
		DbgPrint("Local update timer is disabled: %lf (%d)\n", period, s_info.secured);
		item->timer = NULL;
	}

	s_info.item_list = eina_list_append(s_info.item_list, item);

	if (create_ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (create_ret & OUTPUT_UPDATED) {
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
		if (so_get_output_info(inst, w, h, priority, out_content, out_title) == LB_STATUS_SUCCESS) {
			if (*out_content) {
				char *tmp;

				tmp = strdup(*out_content);
				if (!tmp)
					ErrPrint("Memory: %s\n", strerror(errno));

				*out_content = tmp;
			}

			if (*out_title) {
				char *tmp;

				tmp = strdup(*out_title);
				if (!tmp)
					ErrPrint("Memory: %s\n", strerror(errno));

				*out_title = tmp;
			}
		}
	}

	*w = inst->w;
	*h = inst->h;
	*priority = inst->priority;
	return need_to_create;
}

HAPI int lb_destroy(const char *pkgname, const char *id)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created\n", pkgname, id);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	s_info.item_list = eina_list_remove_list(s_info.item_list, l);

	if (s_info.update == item)
		s_info.update = NULL;

	if (item->timer) {
		clear_from_pd_open_pending_list(item);
		clear_from_pending_list(item);
		ecore_timer_del(item->timer);
		item->timer = NULL;

		if (item->monitor)
			item->deleteme = 1;
		else
			update_monitor_del(id, item);
	}

	if (!item->monitor) {
		free(item);
		(void)so_destroy(inst);
	}

	return LB_STATUS_SUCCESS;
}

HAPI int lb_resize(const char *pkgname, const char *id, int w, int h)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created (%dx%d)\n", pkgname, id, w, h);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, %dx%d)\n", pkgname, id, w, h);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);

	ret = so_resize(inst, w, h);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return LB_STATUS_SUCCESS;
}

HAPI char *lb_pinup(const char *pkgname, const char *id, int pinup)
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

HAPI int lb_set_period(const char *pkgname, const char *id, double period)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not found (period[%lf])\n", pkgname, id, period);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, period[%lf])\n", pkgname, id, period);
		return LB_STATUS_ERROR_NOT_EXIST;
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
				return LB_STATUS_ERROR_FAULT;
			}

			if (s_info.paused)
				timer_freeze(item);
		}
	}

	return LB_STATUS_SUCCESS;
}

HAPI int lb_clicked(const char *pkgname, const char *id, const char *event, double timestamp, double x, double y)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not exists (event[%s])\n", pkgname, id, event);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, event[%s])\n", pkgname, id, event);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);

	ret = so_clicked(inst, event, timestamp, x, y);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return LB_STATUS_SUCCESS;
}

HAPI int lb_script_event(const char *pkgname, const char *id, const char *emission, const char *source, struct event_info *event_info)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not exists (emission[%s], source[%s])\n", pkgname, id, emission, source);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, emissino[%s], source[%s])\n", pkgname, id, emission, source);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);

	if (emission && source && !strcmp(source, id)) {
		if (item->inst->item->has_livebox_script) {
			if (!strcmp(emission, "lb,show")) {
				item->is_lb_show = 1;

				if (item->is_lb_updated && !is_pended_item(item))
					reset_lb_updated_flag(item);

				source = util_uri_to_path(source);
			} else if (!strcmp(emission, "lb,hide")) {
				DbgPrint("Livebox(%s) script is hide now\n", id);
				item->is_lb_show = 0;

				source = util_uri_to_path(source);
			}
		}

		if (!strcmp(emission, "pd,show")) {
			item->is_pd_show = 1;
			source = util_uri_to_path(source);
		} else if (!strcmp(emission, "pd,hide")) {
			item->is_pd_show = 0;
			source = util_uri_to_path(source);
		}
	}

	ret = so_script_event(inst, emission, source, event_info);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return LB_STATUS_SUCCESS;
}

HAPI int lb_is_pinned_up(const char *pkgname, const char *id)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created\n", pkgname, id);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	if (!item) {
		ErrPrint("Invalid item(%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_FAULT;
	}
	/*!
	 * NOTE:
	 * item is not used.
	 * Maybe this is not neccessary for this operation
	 */
	return so_is_pinned_up(inst);
}

HAPI int lb_change_group(const char *pkgname, const char *id, const char *cluster, const char *category)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created (cluster[%s], category[%s])\n", pkgname, id, cluster, category);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s, cluster[%s], category[%s])\n", pkgname, id, cluster, category);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);

	ret = so_change_group(inst, cluster, category);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return LB_STATUS_SUCCESS;
}

static inline int lb_sys_event(struct instance *inst, struct item *item, int event)
{
	int ret;

	ret = so_sys_event(inst, event);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE)
		(void)append_pending_list(item);

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return LB_STATUS_SUCCESS;
}

HAPI int lb_system_event(const char *pkgname, const char *id, int event)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("instance %s - %s is not created\n");
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	return lb_sys_event(inst, item, event);
}

HAPI int lb_update(const char *pkgname, const char *id)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created\n", pkgname, id);
		return LB_STATUS_ERROR_INVALID;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	(void)append_pending_list(item);
	return LB_STATUS_SUCCESS;
}

HAPI int lb_update_all(const char *pkgname, const char *cluster, const char *category)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;

	DbgPrint("Update content for %s\n", pkgname ? pkgname : "(all)");
	EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
		if (item->deleteme)
			continue;

		if (cluster && strcasecmp(item->inst->cluster, cluster))
			continue;

		if (category && strcasecmp(item->inst->category, category))
			continue;

		if (pkgname && strlen(pkgname)) {
			if (!strcmp(item->inst->item->pkgname, pkgname)) {
				(void)append_pending_list(item);
			}
		} else {
			(void)append_pending_list(item);
		}
	}

	return LB_STATUS_SUCCESS;
}

HAPI int lb_delete_all_deleteme(void)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;
	int cnt = 0;

	EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
		if (!item->deleteme)
			continue;

		s_info.item_list = eina_list_remove(s_info.item_list, item);

		update_monitor_del(item->inst->id, item);
		(void)so_destroy(item->inst);
		free(item);
		cnt++;
	}

	DbgPrint("Delete all deleteme: %d\n", cnt);
	return LB_STATUS_SUCCESS;
}

HAPI int lb_system_event_all(int event)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;

	EINA_LIST_FOREACH_SAFE(s_info.item_list, l, n, item) {
		if (item->deleteme)
			continue;

		DbgPrint("System event for %s (%d)\n", item->inst->id, event);
		lb_sys_event(item->inst, item, event);
	}

	return LB_STATUS_SUCCESS;
}

HAPI void lb_pause_all(void)
{
	Eina_List *l;
	struct item *item;

	s_info.paused = 1;

	pending_timer_freeze();

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (item->deleteme) {
			DbgPrint("Instance %s skip timer pause (deleteme)\n", item->inst->item->pkgname);
			continue;
		}

		if (item->is_paused)
			continue;

		timer_freeze(item);

		lb_sys_event(item->inst, item, LB_SYS_EVENT_PAUSED);
	}
}

HAPI void lb_resume_all(void)
{
	Eina_List *l;
	struct item *item;

	s_info.paused = 0;

	pending_timer_thaw();

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (item->deleteme) {
			DbgPrint("Instance %s skip timer resume (deleteme)\n", item->inst->item->pkgname);
			continue;
		}

		if (item->is_paused)
			continue;

		timer_thaw(item);

		lb_sys_event(item->inst, item, LB_SYS_EVENT_RESUMED);

		if (item->updated_in_pause) {
			(void)append_pending_list(item);
			item->updated_in_pause = 0;
		}
	}
}

HAPI int lb_pause(const char *pkgname, const char *id)
{
	struct instance *inst;
	Eina_List *l;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst)
		return LB_STATUS_ERROR_INVALID;

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	if (!item)
		return LB_STATUS_ERROR_FAULT;

	if (item->deleteme) {
		DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
		return LB_STATUS_ERROR_BUSY;
	}

	item->is_paused = 1;

	if (s_info.paused)
		return LB_STATUS_SUCCESS;

	timer_freeze(item);

	lb_sys_event(inst, item, LB_SYS_EVENT_PAUSED);

	return LB_STATUS_SUCCESS;
}

HAPI int lb_resume(const char *pkgname, const char *id)
{
	struct instance *inst;
	Eina_List *l;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst)
		return LB_STATUS_ERROR_INVALID;

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	item = eina_list_data_get(l);
	if (!item)
		return LB_STATUS_ERROR_FAULT;

	if (item->deleteme) {
		DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
		return LB_STATUS_ERROR_BUSY;
	}

	item->is_paused = 0;

	if (s_info.paused)
		return LB_STATUS_SUCCESS;

	timer_thaw(item);

	lb_sys_event(inst, item, LB_SYS_EVENT_RESUMED);

	if (item->updated_in_pause) {
		(void)append_pending_list(item);
		item->updated_in_pause = 0;
	}

	return LB_STATUS_SUCCESS;
}

HAPI void lb_turn_secured_on(void)
{
	s_info.secured = 1;
}

HAPI int lb_is_all_paused(void)
{
	return s_info.paused;
}

/* End of a file */
