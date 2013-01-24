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
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include <Elementary.h>
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_X.h>

#include <dlog.h>
#include <aul.h>
#include <sqlite3.h>

#include <provider.h>

#include "critical_log.h"
#include "conf.h"
#include "debug.h"
#include "client.h"
#include "so_handler.h"
#include "lb.h"
#include "util.h"

static struct info {
	Ecore_Timer *ping_timer;
} s_info = {
	.ping_timer = NULL,
};

static int method_new(struct event_arg *arg, int *width, int *height, double *priority, void *data)
{
	int ret;
	DbgPrint("Create: pkgname[%s], id[%s], content[%s], timeout[%d], has_script[%d], period[%lf], cluster[%s], category[%s], skip[%d], abi[%s]\n",
		arg->pkgname,
		arg->id,
		arg->info.lb_create.content,
		arg->info.lb_create.timeout,
		arg->info.lb_create.has_script,
		arg->info.lb_create.period,
		arg->info.lb_create.cluster, arg->info.lb_create.category,
		arg->info.lb_create.skip_need_to_create,
		arg->info.lb_create.abi);

	ret = lb_create(arg->pkgname, arg->id,
			arg->info.lb_create.content,
			arg->info.lb_create.timeout,
			arg->info.lb_create.has_script,
			arg->info.lb_create.period,
			arg->info.lb_create.cluster,
			arg->info.lb_create.category,
			width, height, priority,
			arg->info.lb_create.skip_need_to_create,
			arg->info.lb_create.abi,
			&arg->info.lb_create.out_content,
			&arg->info.lb_create.out_title);

	if (ret == 0) {
		if (arg->info.lb_create.width > 0 && arg->info.lb_create.height > 0) {
			if (*width != arg->info.lb_create.width || *height != arg->info.lb_create.height) {
				int tmp;
				tmp = lb_resize(arg->pkgname, arg->id, arg->info.lb_create.width, arg->info.lb_create.height);
				DbgPrint("Resize[%dx%d] returns: %d\n", arg->info.lb_create.width, arg->info.lb_create.height, tmp);
			}
		}

		arg->info.lb_create.out_is_pinned_up = (lb_is_pinned_up(arg->pkgname, arg->id) == 1);
	}

	return ret;
}

static int method_renew(struct event_arg *arg, void *data)
{
	 int ret;
	 int w;
	 int h;
	 double priority;

	DbgPrint("Re-create: pkgname[%s], id[%s], content[%s], timeout[%d], has_script[%d], period[%lf], cluster[%s], category[%s], abi[%s]\n",
		arg->pkgname, arg->id,
		arg->info.lb_recreate.content,
		arg->info.lb_recreate.timeout,
		arg->info.lb_recreate.has_script,
		arg->info.lb_recreate.period,
		arg->info.lb_recreate.cluster,
		arg->info.lb_recreate.category,
		arg->info.lb_recreate.abi);

	 ret = lb_create(arg->pkgname, arg->id,
	 		arg->info.lb_recreate.content,
			arg->info.lb_recreate.timeout,
			arg->info.lb_recreate.has_script,
			arg->info.lb_recreate.period,
			arg->info.lb_recreate.cluster,
			arg->info.lb_recreate.category,
			&w, &h, &priority,
			1,
			arg->info.lb_recreate.abi,
			&arg->info.lb_recreate.out_content,
			&arg->info.lb_recreate.out_title);
	if (ret == 0) {
		if (w != arg->info.lb_recreate.width || h != arg->info.lb_recreate.height) {
			int tmp;
			tmp = lb_resize(arg->pkgname, arg->id, arg->info.lb_recreate.width, arg->info.lb_recreate.height);
			DbgPrint("Resize[%dx%d] returns: %d\n", arg->info.lb_recreate.width, arg->info.lb_recreate.height, tmp);
		} else {
			DbgPrint("No need to change the size: %dx%d\n", w, h);
		}

		arg->info.lb_recreate.out_is_pinned_up = (lb_is_pinned_up(arg->pkgname, arg->id) == 1);
	}

	return ret;
}

static int method_delete(struct event_arg *arg, void *data)
{
	int ret;
	DbgPrint("pkgname[%s] id[%s]\n", arg->pkgname, arg->id);
	ret = lb_destroy(arg->pkgname, arg->id);
	return ret;
}

static int method_content_event(struct event_arg *arg, void *data)
{
	int ret;
	struct event_info info;

	info = arg->info.content_event.info;

	ret = lb_script_event(arg->pkgname, arg->id,
				arg->info.content_event.emission, arg->info.content_event.source,
				&info);
	return ret;
}

static int method_clicked(struct event_arg *arg, void *data)
{
	int ret;

	DbgPrint("pkgname[%s] id[%s] event[%s] timestamp[%lf] x[%lf] y[%lf]\n",
								arg->pkgname, arg->id,
								arg->info.clicked.event, arg->info.clicked.timestamp,
								arg->info.clicked.x, arg->info.clicked.y);
	ret = lb_clicked(arg->pkgname, arg->id,
			arg->info.clicked.event,
			arg->info.clicked.timestamp, arg->info.clicked.x, arg->info.clicked.y);

	return ret;
}

static int method_text_signal(struct event_arg *arg, void *data)
{
	int ret;
	struct event_info info;

	info = arg->info.text_signal.info;

	DbgPrint("pkgname[%s] id[%s] emission[%s] source[%s]\n", arg->pkgname, arg->id, arg->info.text_signal.emission, arg->info.text_signal.source);
	ret = lb_script_event(arg->pkgname, arg->id,
				arg->info.text_signal.emission, arg->info.text_signal.source,
				&info);

	return ret;
}

static int method_resize(struct event_arg *arg, void *data)
{
	int ret;

	DbgPrint("pkgname[%s] id[%s] w[%d] h[%d]\n", arg->pkgname, arg->id, arg->info.resize.w, arg->info.resize.h);
	ret = lb_resize(arg->pkgname, arg->id, arg->info.resize.w, arg->info.resize.h);

	return ret;
}

static int method_set_period(struct event_arg *arg, void *data)
{
	int ret;
	DbgPrint("pkgname[%s] id[%s] period[%lf]\n", arg->pkgname, arg->id, arg->info.set_period.period);
	ret = lb_set_period(arg->pkgname, arg->id, arg->info.set_period.period);
	return ret;
}

static int method_change_group(struct event_arg *arg, void *data)
{
	int ret;
	DbgPrint("pkgname[%s] id[%s] cluster[%s] category[%s]\n", arg->pkgname, arg->id, arg->info.change_group.cluster, arg->info.change_group.category);
	ret = lb_change_group(arg->pkgname, arg->id, arg->info.change_group.cluster, arg->info.change_group.category);
	return ret;
}

static int method_pinup(struct event_arg *arg, void *data)
{
	DbgPrint("pkgname[%s] id[%s] state[%d]\n", arg->pkgname, arg->id, arg->info.pinup.state);
	arg->info.pinup.content_info = lb_pinup(arg->pkgname, arg->id, arg->info.pinup.state);
	return arg->info.pinup.content_info ? 0 : -ENOTSUP;
}

static int method_update_content(struct event_arg *arg, void *data)
{
	int ret;

	if (!arg->id || !strlen(arg->id)) {
		DbgPrint("pkgname[%s] cluster[%s] category[%s]\n", arg->pkgname, arg->info.update_content.cluster, arg->info.update_content.category);
		ret = lb_update_all(arg->pkgname, arg->info.update_content.cluster, arg->info.update_content.category);
	} else {
		DbgPrint("Update [%s]\n", arg->id);
		ret = lb_update(arg->pkgname, arg->id);
	}

	return ret;
}

static int method_pause(struct event_arg *arg, void *data)
{
	lb_pause_all();
	if (s_info.ping_timer)
		ecore_timer_freeze(s_info.ping_timer);

	sqlite3_release_memory(SQLITE_FLUSH_MAX);
	malloc_trim(0);
	return 0;
}

static int method_resume(struct event_arg *arg, void *data)
{
	lb_resume_all();
	if (s_info.ping_timer)
		ecore_timer_thaw(s_info.ping_timer);
	return 0;
}

static Eina_Bool send_ping_cb(void *data)
{
	provider_send_ping();
	return ECORE_CALLBACK_RENEW;
}

static int method_disconnected(struct event_arg *arg, void *data)
{
	if (s_info.ping_timer) {
		ecore_timer_del(s_info.ping_timer);
		s_info.ping_timer = NULL;
	}

	elm_exit();
	return 0;
}

static int method_connected(struct event_arg *arg, void *data)
{
	int ret;
	ret = provider_send_hello();
	if (ret == 0) {
		s_info.ping_timer = ecore_timer_add(DEFAULT_PING_TIME, send_ping_cb, NULL);
		if (!s_info.ping_timer)
			ErrPrint("Failed to add a ping timer\n");
	}

	return 0;
}

static int method_pd_created(struct event_arg *arg, void *data)
{
	int ret;

	ret = lb_open_pd(arg->pkgname);
	DbgPrint("%s Open PD: %d\n", arg->pkgname, ret);

	return 0;
}

static int method_pd_destroyed(struct event_arg *arg, void *data)
{
	int ret;

	ret = lb_close_pd(arg->pkgname);
	DbgPrint("%s Close PD: %d\n", arg->pkgname, ret);

	return 0;
}

static int method_lb_pause(struct event_arg *arg, void *data)
{
	return lb_pause(arg->pkgname, arg->id);
}

static int method_lb_resume(struct event_arg *arg, void *data)
{
	return lb_resume(arg->pkgname, arg->id);
}

HAPI int client_init(const char *name)
{
	struct event_handler table = {
		.lb_create = method_new,
		.lb_recreate = method_renew,
		.lb_destroy = method_delete,
		.content_event = method_content_event,
		.clicked = method_clicked,
		.text_signal = method_text_signal,
		.resize = method_resize,
		.set_period = method_set_period,
		.change_group = method_change_group,
		.pinup = method_pinup,
		.update_content = method_update_content,
		.pause = method_pause,
		.resume = method_resume,
		.disconnected = method_disconnected,
		.connected = method_connected,
		.pd_create = method_pd_created,
		.pd_destroy = method_pd_destroyed,
		.lb_pause = method_lb_pause,
		.lb_resume = method_lb_resume,
	};

	return provider_init(ecore_x_display_get(), name, &table, NULL);
}

HAPI int client_fini(void)
{
	(void)provider_fini();
	return 0;
}

/* End of a file */

