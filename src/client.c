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
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include <Elementary.h>
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_X.h>

#include <app.h>
#include <dlog.h>
#include <aul.h>
#include <sqlite3.h>

#include <dynamicbox_provider.h> /* dynamicbox_provider */
#include <dynamicbox_errno.h> /* dynamicbox_service */
#include <dynamicbox_script.h> /* dynamicbox_service - dynamicbox_event_info */
#include <dynamicbox_conf.h>
#include <internal/dynamicbox.h> /* dynamicbox - DBOX_SYS_EVENT_XXX */
#include <dynamicbox.h> /* dynamicbox - DBOX_SYS_EVENT_XXX */

#include "critical_log.h"
#include "debug.h"
#include "client.h"
#include "so_handler.h"
#include "dbox.h"
#include "util.h"
#include "conf.h"

static struct info {
    Ecore_Timer *ping_timer;
} s_info = {
    .ping_timer = NULL,
};

static int method_new(struct dynamicbox_event_arg *arg, int *width, int *height, double *priority, void *data)
{
    int ret;
    struct dbox_create_arg _arg;
    DbgPrint("Create: pkgname[%s], id[%s], content[%s], timeout[%d], has_script[%d], period[%lf], cluster[%s], category[%s], skip[%d], abi[%s], size: %dx%d\n",
	    arg->pkgname,
	    arg->id,
	    arg->info.dbox_create.content,
	    arg->info.dbox_create.timeout,
	    arg->info.dbox_create.has_script,
	    arg->info.dbox_create.period,
	    arg->info.dbox_create.cluster, arg->info.dbox_create.category,
	    arg->info.dbox_create.skip_need_to_create,
	    arg->info.dbox_create.abi,
	    arg->info.dbox_create.width,
	    arg->info.dbox_create.height);

    if (!arg->info.dbox_create.content || !strlen(arg->info.dbox_create.content)) {
	DbgPrint("Use default content: \"%s\"\n", DYNAMICBOX_CONF_DEFAULT_CONTENT);
	arg->info.dbox_create.content = DYNAMICBOX_CONF_DEFAULT_CONTENT;
    }

    _arg.content = arg->info.dbox_create.content;
    _arg.timeout = arg->info.dbox_create.timeout;
    _arg.has_dynamicbox_script = arg->info.dbox_create.has_script;
    _arg.period = arg->info.dbox_create.period;
    _arg.cluster = arg->info.dbox_create.cluster;
    _arg.category = arg->info.dbox_create.category;
    _arg.abi = arg->info.dbox_create.abi;
    _arg.skip_need_to_create = arg->info.dbox_create.skip_need_to_create;
    _arg.direct_addr = arg->info.dbox_create.direct_addr;

    ret = dbox_create(arg->pkgname, arg->id,
	    &_arg,
	    width, height, priority,
	    &arg->info.dbox_create.out_content,
	    &arg->info.dbox_create.out_title);

    if (ret == 0) {
	if (arg->info.dbox_create.width > 0 && arg->info.dbox_create.height > 0) {
	    DbgPrint("Create size: %dx%d (created: %dx%d)\n", arg->info.dbox_create.width, arg->info.dbox_create.height, *width, *height);
	    if (*width != arg->info.dbox_create.width || *height != arg->info.dbox_create.height) {
		int tmp;
		tmp = dbox_resize(arg->pkgname, arg->id, arg->info.dbox_create.width, arg->info.dbox_create.height);
		DbgPrint("dbox_resize returns: %d\n", tmp);
		if (tmp == (int)DBOX_STATUS_ERROR_NONE) {
		    /*!
		     * \note
		     * Just returns resized canvas size.
		     * Even if it is not ready to render contents.
		     * Provider will allocate render buffer using this size.
		     */
		    *width = arg->info.dbox_create.width;
		    *height = arg->info.dbox_create.height;
		}
	    }
	}

	arg->info.dbox_create.out_is_pinned_up = (dbox_is_pinned_up(arg->pkgname, arg->id) == 1);
    } else {
	ErrPrint("dbox_create returns %d\n", ret);
    }

    if (dbox_is_all_paused()) {
	DbgPrint("Box is paused\n");
	(void)dbox_system_event(arg->pkgname, arg->id, DBOX_SYS_EVENT_PAUSED);
    }

    return ret;
}

static int method_renew(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    int w;
    int h;
    double priority;
    struct dbox_create_arg _arg;

    DbgPrint("Re-create: pkgname[%s], id[%s], content[%s], timeout[%d], has_script[%d], period[%lf], cluster[%s], category[%s], abi[%s]\n",
	    arg->pkgname, arg->id,
	    arg->info.dbox_recreate.content,
	    arg->info.dbox_recreate.timeout,
	    arg->info.dbox_recreate.has_script,
	    arg->info.dbox_recreate.period,
	    arg->info.dbox_recreate.cluster,
	    arg->info.dbox_recreate.category,
	    arg->info.dbox_recreate.abi);

    if (!arg->info.dbox_recreate.content || !strlen(arg->info.dbox_recreate.content)) {
	DbgPrint("Use default content: \"%s\"\n", DYNAMICBOX_CONF_DEFAULT_CONTENT);
	arg->info.dbox_recreate.content = DYNAMICBOX_CONF_DEFAULT_CONTENT;
    }

    _arg.content = arg->info.dbox_recreate.content;
    _arg.timeout = arg->info.dbox_recreate.timeout;
    _arg.has_dynamicbox_script = arg->info.dbox_recreate.has_script;
    _arg.period = arg->info.dbox_recreate.period;
    _arg.cluster = arg->info.dbox_recreate.cluster;
    _arg.category = arg->info.dbox_recreate.category;
    _arg.abi = arg->info.dbox_recreate.abi;
    _arg.skip_need_to_create = 1;
    _arg.direct_addr = arg->info.dbox_recreate.direct_addr;

    ret = dbox_create(arg->pkgname, arg->id,
	    &_arg,
	    &w, &h, &priority,
	    &arg->info.dbox_recreate.out_content,
	    &arg->info.dbox_recreate.out_title);
    if (ret == 0) {
	if (w != arg->info.dbox_recreate.width || h != arg->info.dbox_recreate.height) {
	    int tmp;
	    tmp = dbox_resize(arg->pkgname, arg->id, arg->info.dbox_recreate.width, arg->info.dbox_recreate.height);
	    if (tmp < 0) {
		DbgPrint("Resize[%dx%d] returns: %d\n", arg->info.dbox_recreate.width, arg->info.dbox_recreate.height, tmp);
	    }
	} else {
	    DbgPrint("No need to change the size: %dx%d\n", w, h);
	}

	arg->info.dbox_recreate.out_is_pinned_up = (dbox_is_pinned_up(arg->pkgname, arg->id) == 1);
    } else {
	ErrPrint("dbox_create returns %d\n", ret);
    }

    if (dbox_is_all_paused()) {
	DbgPrint("Box is paused\n");
	(void)dbox_system_event(arg->pkgname, arg->id, DBOX_SYS_EVENT_PAUSED);
    }

    return ret;
}

static int method_delete(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    DbgPrint("pkgname[%s] id[%s]\n", arg->pkgname, arg->id);

    if (arg->info.dbox_destroy.type == DBOX_DESTROY_TYPE_DEFAULT || arg->info.dbox_destroy.type == DBOX_DESTROY_TYPE_UNINSTALL) {
	DbgPrint("Box is deleted from the viewer\n");
	(void)dbox_system_event(arg->pkgname, arg->id, DBOX_SYS_EVENT_DELETED);
    }

    ret = dbox_destroy(arg->pkgname, arg->id, 0);
    return ret;
}

static int method_content_event(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    struct dynamicbox_event_info info;

    info = arg->info.content_event.info;

    ret = dbox_script_event(arg->pkgname, arg->id,
	    arg->info.content_event.emission, arg->info.content_event.source,
	    &info);
    return ret;
}

static int method_clicked(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    DbgPrint("pkgname[%s] id[%s] event[%s] timestamp[%lf] x[%lf] y[%lf]\n",
	    arg->pkgname, arg->id,
	    arg->info.clicked.event, arg->info.clicked.timestamp,
	    arg->info.clicked.x, arg->info.clicked.y);
    ret = dbox_clicked(arg->pkgname, arg->id,
	    arg->info.clicked.event,
	    arg->info.clicked.timestamp, arg->info.clicked.x, arg->info.clicked.y);

    return ret;
}

static int method_text_signal(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    struct dynamicbox_event_info info;

    info = arg->info.text_signal.info;

    DbgPrint("pkgname[%s] id[%s] emission[%s] source[%s]\n", arg->pkgname, arg->id, arg->info.text_signal.emission, arg->info.text_signal.source);
    ret = dbox_script_event(arg->pkgname, arg->id,
	    arg->info.text_signal.emission, arg->info.text_signal.source,
	    &info);

    return ret;
}

static int method_resize(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    DbgPrint("pkgname[%s] id[%s] w[%d] h[%d]\n", arg->pkgname, arg->id, arg->info.resize.w, arg->info.resize.h);
    ret = dbox_resize(arg->pkgname, arg->id, arg->info.resize.w, arg->info.resize.h);

    return ret;
}

static int method_set_period(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    DbgPrint("pkgname[%s] id[%s] period[%lf]\n", arg->pkgname, arg->id, arg->info.set_period.period);
    ret = dbox_set_period(arg->pkgname, arg->id, arg->info.set_period.period);
    return ret;
}

static int method_change_group(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    DbgPrint("pkgname[%s] id[%s] cluster[%s] category[%s]\n", arg->pkgname, arg->id, arg->info.change_group.cluster, arg->info.change_group.category);
    ret = dbox_change_group(arg->pkgname, arg->id, arg->info.change_group.cluster, arg->info.change_group.category);
    return ret;
}

static int method_pinup(struct dynamicbox_event_arg *arg, void *data)
{
    DbgPrint("pkgname[%s] id[%s] state[%d]\n", arg->pkgname, arg->id, arg->info.pinup.state);
    arg->info.pinup.content_info = dbox_pinup(arg->pkgname, arg->id, arg->info.pinup.state);
    return arg->info.pinup.content_info ? DBOX_STATUS_ERROR_NONE : DBOX_STATUS_ERROR_NOT_IMPLEMENTED;
}

static int method_update_content(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    if (!arg->id || !strlen(arg->id)) {
	if (arg->info.update_content.content && strlen(arg->info.update_content.content)) {
	    DbgPrint("pkgname[%s] content[%s]\n", arg->pkgname, arg->info.update_content.content);
	    ret = dbox_set_content_info_all(arg->pkgname, arg->info.update_content.content);
	} else {
	    DbgPrint("pkgname[%s] cluster[%s] category[%s]\n", arg->pkgname, arg->info.update_content.cluster, arg->info.update_content.category);
	    ret = dbox_update_all(arg->pkgname, arg->info.update_content.cluster, arg->info.update_content.category, arg->info.update_content.force);
	}
    } else {
	if (arg->info.update_content.content && strlen(arg->info.update_content.content)) {
	    DbgPrint("id[%s] content[%s]\n", arg->id, arg->info.update_content.content);
	    ret = dbox_set_content_info(arg->pkgname, arg->id, arg->info.update_content.content);
	} else {
	    DbgPrint("Update [%s]\n", arg->id);
	    ret = dbox_update(arg->pkgname, arg->id, arg->info.update_content.force);
	}
    }

    return ret;
}

static int method_pause(struct dynamicbox_event_arg *arg, void *data)
{
    dbox_pause_all();

    if (s_info.ping_timer) {
	ecore_timer_freeze(s_info.ping_timer);
    }

    if (DYNAMICBOX_CONF_SLAVE_AUTO_CACHE_FLUSH) {
	elm_cache_all_flush();
	sqlite3_release_memory(DYNAMICBOX_CONF_SQLITE_FLUSH_MAX);
	malloc_trim(0);
    }

    return DBOX_STATUS_ERROR_NONE;
}

static int method_resume(struct dynamicbox_event_arg *arg, void *data)
{
    dbox_resume_all();
    if (s_info.ping_timer) {
	ecore_timer_thaw(s_info.ping_timer);
    }
    return DBOX_STATUS_ERROR_NONE;
}

static Eina_Bool send_ping_cb(void *data)
{
    dynamicbox_provider_send_ping();
    return ECORE_CALLBACK_RENEW;
}

static int method_disconnected(struct dynamicbox_event_arg *arg, void *data)
{
    if (s_info.ping_timer) {
	ecore_timer_del(s_info.ping_timer);
	s_info.ping_timer = NULL;
    }

    ui_app_exit();
    return DBOX_STATUS_ERROR_NONE;
}

static int method_connected(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    ret = dynamicbox_provider_send_hello();
    if (ret == 0) {
	double ping_interval;

	ping_interval = DYNAMICBOX_CONF_DEFAULT_PING_TIME / 2.0f;
	DbgPrint("Ping Timer: %lf\n", ping_interval);

	s_info.ping_timer = ecore_timer_add(ping_interval, send_ping_cb, NULL);
	if (!s_info.ping_timer) {
	    ErrPrint("Failed to add a ping timer\n");
	}
    }

    return DBOX_STATUS_ERROR_NONE;
}

static int method_gbar_created(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    ret = dbox_open_gbar(arg->pkgname, arg->id);
    if (ret < 0) {
	DbgPrint("%s Open PD: %d\n", arg->id, ret);
    }

    return DBOX_STATUS_ERROR_NONE;
}

static int method_gbar_destroyed(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    ret = dbox_close_gbar(arg->pkgname, arg->id);
    if (ret < 0) {
	DbgPrint("%s Close PD: %d\n", arg->id, ret);
    }

    return DBOX_STATUS_ERROR_NONE;
}

static int method_gbar_moved(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;
    struct dynamicbox_event_info info;

    memset(&info, 0, sizeof(info));
    info.pointer.x = arg->info.gbar_move.x;
    info.pointer.y = arg->info.gbar_move.y;
    info.pointer.down = 0;

    ret = dbox_script_event(arg->pkgname, arg->id,
	    "gbar,move", util_uri_to_path(arg->id), &info);
    return ret;
}

static int method_dbox_pause(struct dynamicbox_event_arg *arg, void *data)
{
    int ret;

    ret = dbox_pause(arg->pkgname, arg->id);

    if (DYNAMICBOX_CONF_SLAVE_AUTO_CACHE_FLUSH) {
	elm_cache_all_flush();
	sqlite3_release_memory(DYNAMICBOX_CONF_SQLITE_FLUSH_MAX);
	malloc_trim(0);
    }

    return ret;
}

static int method_dbox_resume(struct dynamicbox_event_arg *arg, void *data)
{
    return dbox_resume(arg->pkgname, arg->id);
}

HAPI int client_init(const char *name)
{
    struct dynamicbox_event_table table = {
	.dbox_create = method_new,
	.dbox_recreate = method_renew,
	.dbox_destroy = method_delete,
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
	.gbar_create = method_gbar_created,
	.gbar_destroy = method_gbar_destroyed,
	.gbar_move = method_gbar_moved,
	.dbox_pause = method_dbox_pause,
	.dbox_resume = method_dbox_resume,
    };

    return dynamicbox_provider_init(util_screen_get(), name, &table, NULL, 1, 1);
}

HAPI int client_fini(void)
{
    (void)dynamicbox_provider_fini();
    return DBOX_STATUS_ERROR_NONE;
}

/* End of a file */

