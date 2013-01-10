/*
 * Copyright 2012  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.tizenopensource.org/license
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

#include <Ecore.h>
#include <Eina.h>

#include <dlog.h>
#include <provider.h>
#include <livebox.h>

#include "critical_log.h"
#include "debug.h"
#include "conf.h"
#include "so_handler.h"
#include "lb.h"
#include "update_monitor.h"
#include "fault.h"
#include "util.h"

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
	DbgPrint("Consuming pended item: %s\n", item->inst->id);
	/*!
	 * \note
	 * To prevent from checking the is_updated function
	 */
	(void)updator_cb(item);
	if (s_info.pd_open_pending_list)
		return ECORE_CALLBACK_RENEW;

cleanout:
	s_info.pd_open_pending_timer = NULL;
	DbgPrint("open pd pending list exhausted\n");
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
	DbgPrint("Consuming pended item: %s\n", item->inst->id);
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
	DbgPrint("pending list exhausted\n");
	return ECORE_CALLBACK_CANCEL;
}

static inline __attribute__((always_inline)) int activate_pending_consumer(void)
{
	if (s_info.pending_timer)
		return 0;

	s_info.pending_timer = ecore_timer_add(0.000001f, pended_cmd_consumer_cb, NULL);
	if (!s_info.pending_timer) {
		ErrPrint("Failed to add a new pended command consumer\n");
		return -EFAULT;
	}

	/*!
	 * Do not increase the freezed counter.
	 * Just freeze the timer.
	 */
	if (s_info.pending_timer_freezed) {
		DbgPrint("Pending timer created and freezed\n");
		ecore_timer_freeze(s_info.pending_timer);
	}

	return 0;
}

static inline void deactivate_pending_consumer(void)
{
	if (!s_info.pending_timer)
		return;

	ecore_timer_del(s_info.pending_timer);
	s_info.pending_timer = NULL;
	s_info.pending_timer_freezed = 0;
	DbgPrint("Clear the pending timer\n");
}

static inline void deactivate_pd_open_pending_consumer(void)
{
	if (!s_info.pd_open_pending_timer)
		return;

	ecore_timer_del(s_info.pd_open_pending_timer);
	s_info.pd_open_pending_timer = NULL;
	DbgPrint("Clear the open_pd_pending timer\n");
}

static inline int __attribute__((always_inline)) activate_pd_open_pending_consumer(void)
{
	if (s_info.pd_open_pending_timer)
		return 0;

	s_info.pd_open_pending_timer = ecore_timer_add(0.000001f, pd_open_pended_cmd_consumer_cb, NULL);
	if (!s_info.pd_open_pending_timer) {
		ErrPrint("Failed to add a new pended command consumer\n");
		return -EFAULT;
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

	DbgPrint("%d items are migrated\n", cnt);
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

	DbgPrint("%d items are migrated\n", cnt);
}

static inline int append_pending_list(struct item *item)
{
	if (pd_is_opened(item->inst->item->pkgname) == 1) {
		if (eina_list_data_find(s_info.pd_open_pending_list, item) == item) {
			DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
			return -EEXIST;
		}

		if (activate_pd_open_pending_consumer() < 0)
			return -EFAULT;

		s_info.pd_open_pending_list = eina_list_append(s_info.pd_open_pending_list, item);
	} else {
		if (eina_list_data_find(s_info.pending_list, item) == item) {
			DbgPrint("Already pended - %s\n", item->inst->item->pkgname);
			return -EEXIST;
		}

		if (activate_pending_consumer() < 0)
			return -EFAULT;

		s_info.pending_list = eina_list_append(s_info.pending_list, item);
	}
	return 0;
}

static inline void timer_thaw(struct item *item)
{
	struct timeval tv;
	double pending;
	double compensate;
	double sleep_time;

	ecore_timer_thaw(item->timer);

	if (item->sleep_at == 0.0f)
		return;

	pending = ecore_timer_pending_get(item->timer);

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("Failed to get timeofday: %s\n", strerror(errno));
		return;
	}
	sleep_time = ((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f) - item->sleep_at;
	compensate = 60.0f - ((double)(tv.tv_sec % 60) + ((double)tv.tv_usec / 1000000.0f));

	ecore_timer_delay(item->timer, compensate - pending);
	DbgPrint("Compensate timer: %lf\n", compensate - pending);

	if (sleep_time > pending) {
		DbgPrint("Append pending list to update content\n");
		(void)append_pending_list(item);
	}

	item->sleep_at = 0.0f;
}

static inline void timer_freeze(struct item *item)
{
	struct timeval tv;
	ecore_timer_freeze(item->timer);

	if (ecore_timer_interval_get(item->timer) <= 1.0f)
		return;

	gettimeofday(&tv, NULL);
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
	if (interval >= MINIMUM_UPDATE_INTERVAL)
		item->monitor_cnt++;
	else
		item->heavy_updating = 1;

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
	provider_send_desc_updated(item->inst->item->pkgname, item->inst->id, filename);
	return EXIT_SUCCESS;
}

static int file_updated_cb(const char *filename, void *data, int over)
{
	struct item *item;
	int w;
	int h;
	double priority;
	char *content;
	char *title;

	if (over)
		WarnPrint("Event Q overflow\n");

	item = data;

	(void)so_get_output_info(item->inst, &w, &h, &priority, &content, &title);
	provider_send_updated(item->inst->item->pkgname, item->inst->id,
					item->inst->w, item->inst->h, item->inst->priority, content, title);

	return output_handler(item);
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
		return 0;
	}

	return -ENOENT;
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
		return 0;
	}

	return -ENOENT;
}

static Eina_Bool update_timeout_cb(void *data)
{
	struct item *item;

	item = data;

	DbgPrint("UPDATE TIMEOUT ========> %s - %s\n", item->inst->item->pkgname, item->inst->id);

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

	ret = so_is_updated(item->inst);
	if (ret <= 0) {
		if (so_need_to_destroy(item->inst) == NEED_TO_DESTROY) {
			provider_send_deleted(item->inst->item->pkgname, item->inst->id);
			lb_destroy(item->inst->item->pkgname, item->inst->id);

			ecore_timer_del(item->timer);
			item->timer = NULL;
			return ECORE_CALLBACK_CANCEL;
		}

		return ECORE_CALLBACK_RENEW;
	}

	if (s_info.update || pd_is_opened(item->inst->item->pkgname) < 0) {
		(void)append_pending_list(item);
		return ECORE_CALLBACK_RENEW;
	}

	item->monitor = ecore_timer_add(item->inst->item->timeout, update_timeout_cb, item);
	if (!item->monitor) {
		ErrPrint("Failed to add update monitor %s(%s):%d\n",
					item->inst->item->pkgname, item->inst->id, item->inst->item->timeout);
		return ECORE_CALLBACK_RENEW;
	}

	/*!
	 * \note
	 * Counter of the event monitor is only used for asynchronous content updating,
	 * So reset it to 1 from here because the async updating is started now,
	 * even if it is accumulated by other event function before this.
	 */
	item->monitor_cnt = 1;

	s_info.update = item;

	ret = so_update(item->inst);
	if (ret < 0) {
		item->monitor_cnt--;

		ecore_timer_del(item->monitor);
		item->monitor = NULL;
		s_info.update = NULL;
		return ECORE_CALLBACK_RENEW;
	}

	/*!
	 * \note
	 * While waiting the Callback function call,
	 * Add this for finding the crash
	 */
	fault_mark_call(item->inst->item->pkgname, item->inst->id, "update,crashed", NO_ALARM, DEFAULT_LIFE_TIMER);

	if (ret & NEED_TO_SCHEDULE) {
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return ECORE_CALLBACK_RENEW;
}

static inline void update_monitor_del(struct item *item)
{
	char *tmp;
	int len;

	update_monitor_del_update_cb(util_uri_to_path(item->inst->id), file_updated_cb);

	len = strlen(util_uri_to_path(item->inst->id)) + strlen(".desc") + 1;
	tmp = malloc(len);
	if (!tmp) {
		ErrPrint("Heap: %s (%s.desc)\n", strerror(errno), util_uri_to_path(item->inst->id));
		return;
	}

	snprintf(tmp, len, "%s.desc", util_uri_to_path(item->inst->id));
	update_monitor_del_update_cb(tmp, desc_updated_cb);
	free(tmp);
}

static inline int add_desc_update_monitor(struct item *item)
{
	char *filename;
	int len;

	len = strlen(util_uri_to_path(item->inst->id)) + strlen(".desc") + 1;
	filename = malloc(len);
	if (!filename) {
		ErrPrint("Heap: %s (%s.desc)\n", strerror(errno), util_uri_to_path(item->inst->id));
		return -ENOMEM;
	}

	snprintf(filename, len, "%s.desc", util_uri_to_path(item->inst->id));
	DbgPrint("Add DESC monitor: %s\n", filename);
	return update_monitor_add_update_cb(filename, desc_updated_cb, item);
}

static inline int add_file_update_monitor(struct item *item)
{
	char *filename;

	filename = strdup(util_uri_to_path(item->inst->id));
	if (!filename) {
		ErrPrint("Heap: %s (%s)\n", strerror(errno), item->inst->id);
		return -ENOMEM;
	}

	return update_monitor_add_update_cb(filename, file_updated_cb, item);
}

static inline int update_monitor_add(struct item *item)
{
	add_file_update_monitor(item);
	add_desc_update_monitor(item);
	return 0;
}

HAPI int lb_init(void)
{
	return 0;
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

	return 0;
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

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (!strcmp(item->inst->id, filename)) {
			return append_pending_list(item);
		}
	}

	return -ENOENT;
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
		return -ENOMEM;
	}

	if (!s_info.pd_list)
		pending_timer_freeze();

	s_info.pd_list = eina_list_append(s_info.pd_list, tmp);

	/*!
	 * Find all instances from the pending list.
	 * Move them to pd_open_pending_timer
	 */
	migrate_to_pd_open_pending_list(pkgname);
	return 0;
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
		return 0;
	}

	return -ENOENT;
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
		return 0;
	}

	if (!skip_need_to_create) {
		ret = so_create_needed(pkgname, cluster, category, abi);
		if (ret != NEED_TO_CREATE)
			return -EPERM;

		need_to_create = 1;
	}

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s (%s - %s, content[%s], cluster[%s], category[%s], abi[%s])\n", strerror(errno), pkgname, id, content_info, cluster, category, abi);
		return -ENOMEM;
	}

	item->monitor = NULL;
	item->monitor_cnt = 0;
	item->deleteme = 0;
	item->update_interval = 0.0f;
	item->heavy_updating = 0;
	item->is_paused = 0;
	item->sleep_at = 0.0f;

	create_ret = so_create(pkgname, id, content_info, timeout, has_livebox_script, cluster, category, abi, &inst);
	if (create_ret < 0) {
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
			so_destroy(inst);
			free(item);
			return -EFAULT;
		}

		if (s_info.paused)
			timer_freeze(item);
	} else {
		DbgPrint("Local update timer is disabled: %lf (%d)\n", period, s_info.secured);
		item->timer = NULL;
	}

	ret = update_monitor_add(item);
	if (ret < 0) {
		so_destroy(inst);
		if (item->timer)
			ecore_timer_del(item->timer);
		free(item);
		return ret;
	}

	s_info.item_list = eina_list_append(s_info.item_list, item);

	if (create_ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (create_ret & OUTPUT_UPDATED) {
		char *tmp_content;
		char *tmp_title;

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
		(void)so_get_output_info(inst, w, h, priority, &tmp_content, &tmp_title);

		/*!
		 * \note
		 * These two values will be released by the provider library.
		 */
		if (tmp_content) {
			*out_content = strdup(tmp_content);
			if (!*out_content)
				ErrPrint("Heap: %s\n", strerror(errno));
		}

		if (tmp_title) {
			*out_title = strdup(tmp_title);
			if (!*out_title)
				ErrPrint("Heap: %s\n", strerror(errno));
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
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created\n", pkgname, id);
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return -ENOENT;
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
			update_monitor_del(item);
	}

	if (!item->monitor) {
		free(item);
		ret = so_destroy(inst);
	}

	return 0;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, %dx%d)\n", pkgname, id, w, h);
		return -ENOENT;
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

	return 0;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, period[%lf])\n", pkgname, id, period);
		return -ENOENT;
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
				return -EFAULT;
			}

			if (s_info.paused)
				timer_freeze(item);
		}
	}

	return 0;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, event[%s])\n", pkgname, id, event);
		return -ENOENT;
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

	return 0;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s, emissino[%s], source[%s])\n", pkgname, id, emission, source);
		return -ENOENT;
	}

	item = eina_list_data_get(l);

	ret = so_script_event(inst, emission, source, event_info);
	if (ret < 0)
		return ret;

	if (ret & NEED_TO_SCHEDULE) {
		DbgPrint("%s Returns NEED_TO_SCHEDULE\n", pkgname);
		(void)append_pending_list(item);
	}

	if (ret & OUTPUT_UPDATED)
		update_monitor_cnt(item);

	return 0;
}

HAPI int lb_is_pinned_up(const char *pkgname, const char *id)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;
	int ret;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("Instance %s - %s is not created\n", pkgname, id);
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return -ENOENT;
	}

	item = eina_list_data_get(l);
	/*!
	 * NOTE:
	 * item is not used.
	 * Maybe this is not neccessary for this operation
	 */
	ret = so_is_pinned_up(inst);
	return ret;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s, cluster[%s], category[%s])\n", pkgname, id, cluster, category);
		return -ENOENT;
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

	return 0;
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

	return 0;
}

HAPI int lb_system_event(const char *pkgname, const char *id, int event)
{
	Eina_List *l;
	struct instance *inst;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst) {
		ErrPrint("instance %s - %s is not created\n");
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return -ENOENT;
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
		return -EINVAL;
	}

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found(%s - %s)\n", pkgname, id);
		return -ENOENT;
	}

	item = eina_list_data_get(l);
	(void)append_pending_list(item);
	return 0;
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

	return 0;
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

	return 0;
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

		if (item->is_paused) {
			DbgPrint("Instance %s is already paused\n", item->inst->id);
			continue;
		}

		if (item->timer) {
			DbgPrint("Instance %s freeze timer\n", item->inst->item->pkgname);
			timer_freeze(item);
		} else {
			DbgPrint("Instance %s has no timer\n", item->inst->item->pkgname);
		}

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

		if (item->is_paused) {
			DbgPrint("Instance %s is still paused\n", item->inst->id);
			continue;
		}

		if (item->timer) {
			DbgPrint("Instance %s resume timer\n", item->inst->item->pkgname);
			timer_thaw(item);
		}

		lb_sys_event(item->inst, item, LB_SYS_EVENT_RESUMED);
	}
}

HAPI int lb_pause(const char *pkgname, const char *id)
{
	struct instance *inst;
	Eina_List *l;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst)
		return -EINVAL;

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return -ENOENT;
	}

	item = eina_list_data_get(l);
	if (!item)
		return -EFAULT;

	if (item->deleteme) {
		DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
		return -EBUSY;
	}

	item->is_paused = 1;

	if (s_info.paused) {
		DbgPrint("Already paused: %s\n", item->inst->id);
		return 0;
	}

	if (item->timer)
		timer_freeze(item);

	lb_sys_event(inst, item, LB_SYS_EVENT_PAUSED);

	return 0;
}

HAPI int lb_resume(const char *pkgname, const char *id)
{
	struct instance *inst;
	Eina_List *l;
	struct item *item;

	inst = so_find_instance(pkgname, id);
	if (!inst)
		return -EINVAL;

	l = find_item(inst);
	if (!l) {
		ErrPrint("Instance is not found (%s - %s)\n", pkgname, id);
		return -ENOENT;
	}

	item = eina_list_data_get(l);
	if (!item)
		return -EFAULT;

	if (item->deleteme) {
		DbgPrint("Instance %s will be deleted (%s)\n", item->inst->item->pkgname, item->inst->id);
		return -EBUSY;
	}

	item->is_paused = 0;

	if (s_info.paused) {
		DbgPrint("Instance %s is still paused\n", item->inst->id);
		return 0;
	}

	if (item->timer)
		timer_thaw(item);

	lb_sys_event(inst, item, LB_SYS_EVENT_RESUMED);
	return 0;
}

HAPI void lb_turn_secured_on(void)
{
	s_info.secured = 1;
}

/* End of a file */
