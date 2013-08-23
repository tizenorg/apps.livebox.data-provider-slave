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
#include <errno.h>
#include <dlfcn.h> /* dlopen */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strcmp */

#include <dlog.h>
#include <Eina.h>
#include <provider.h>
#include <livebox-service.h>
#include <livebox-errno.h>

#include "main.h"
#include "critical_log.h"
#include "debug.h"
#include "so_handler.h"
#include "fault.h"
#include "conf.h"
#include "util.h"

int errno;

static struct info {
	Eina_List *livebox_list;
	enum current_operations current_op;
} s_info = {
	.livebox_list = NULL,
	.current_op = LIVEBOX_OP_UNKNOWN,
};

static inline struct so_item *find_livebox(const char *pkgname)
{
	Eina_List *l;
	struct so_item *item;

	EINA_LIST_FOREACH(s_info.livebox_list, l, item) {
		if (!strcmp(item->pkgname, pkgname)) {
			return item;
		}
	}

	return NULL;
}

static inline char *so_adaptor_alloc(const char *abi)
{
	/* TODO: Implement me */
	DbgPrint("ABI[%s] loads %s\n", abi, "/usr/lib/liblivebox-cpp.so");
	return strdup("/usr/lib/liblivebox-cpp.so");
}

static inline char *old_style_path(const char *pkgname)
{
	char *path;
	int path_len;
	int ret;

	path_len = (strlen(pkgname) * 2) + strlen(MODULE_PATH);
	path = malloc(path_len);
	if (!path) {
		ErrPrint("Memory: %s\n", strerror(errno));
		return NULL;
	}

	ret = snprintf(path, path_len, MODULE_PATH, pkgname, pkgname);
	if (ret < 0) {
		ErrPrint("Fault: %s\n", strerror(errno));
		free(path);
		return NULL;
	}

	DbgPrint("Fallback to old style libexec path (%s)\n", path);
	return path;
}

static inline char *so_path_alloc(const char *pkgname)
{
	char *lb_pkgname;
	char *path;

	lb_pkgname = livebox_service_pkgname(pkgname);
	if (!lb_pkgname) {
		path = old_style_path(pkgname);
	} else {
		path = livebox_service_libexec(lb_pkgname);
		free(lb_pkgname);
	}

	DbgPrint("so path: %s\n", path);
	return path;
}

static void delete_livebox(struct so_item *item)
{
	if (item->adaptor.finalize) {
		int ret;
		fault_mark_call(item->pkgname, "finalize", __func__, USE_ALARM, DEFAULT_LIFE_TIMER);
		ret = item->adaptor.finalize(item->pkgname);
		fault_unmark_call(item->pkgname, "finalize", __func__, USE_ALARM);

		ErrPrint("Package %s, finalize returns %d\n", item->pkgname, ret);
	} else if (item->livebox.finalize) {
		int ret;
		fault_mark_call(item->pkgname, "finalize", __func__, USE_ALARM, DEFAULT_LIFE_TIMER);
		ret = item->livebox.finalize();
		fault_unmark_call(item->pkgname, "finalize", __func__, USE_ALARM);

		ErrPrint("Package %s, finalize returns %d\n", item->pkgname, ret);
	}

	main_heap_monitor_del_target(item->so_fname);
	if (dlclose(item->handle) != 0) {
		ErrPrint("dlclose: %s\n", dlerror());
	}
	free(item->so_fname);
	free(item->pkgname);
	free(item);
}

static struct so_item *new_adaptor(const char *pkgname, const char *abi)
{
	struct so_item *item;
	char *errmsg;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Memory: %s\n", strerror(errno));
		return NULL;
	}

	item->pkgname = strdup(pkgname);
	if (!item->pkgname) {
		ErrPrint("Memory: %s\n", strerror(errno));
		free(item);
		return NULL;
	}

	/*! \TODO:
	 * item->timeout
	 */

	/*! \TODO
	 * item->has_livbox_script
	 */

	item->inst_list = NULL;

	item->so_fname = so_adaptor_alloc(abi);
	if (!item->so_fname) {
		free(item->pkgname);
		free(item);
		return NULL;
	}

	fault_mark_call(pkgname, __func__, __func__, USE_ALARM, DEFAULT_LOAD_TIMER);
	item->handle = dlopen(item->so_fname, RTLD_LOCAL | RTLD_NOW | RTLD_DEEPBIND);
	if (!item->handle) {
		fault_unmark_call(pkgname, __func__, __func__, USE_ALARM);
		ErrPrint("dlopen: %s - %s\n", dlerror(), item->so_fname);
		free(item->so_fname);
		free(item->pkgname);
		free(item);
		return NULL;
	}
	fault_unmark_call(pkgname, __func__, __func__, USE_ALARM);

	errmsg = dlerror();
	if (errmsg) {
		DbgPrint("dlerror(can be ignored): %s\n", errmsg);
	}

	item->adaptor.create = (adaptor_create_t)dlsym(item->handle, "livebox_create");
	if (!item->adaptor.create) {
		ErrPrint("symbol: livebox_create - %s\n", dlerror());
		delete_livebox(item);
		return NULL;
	}

	item->adaptor.destroy = (adaptor_destroy_t)dlsym(item->handle, "livebox_destroy");
	if (!item->adaptor.destroy) {
		ErrPrint("symbol: livebox_destroy - %s\n", dlerror());
		delete_livebox(item);
		return NULL;
	}

	item->adaptor.pinup = (adaptor_pinup_t)dlsym(item->handle, "livebox_pinup");
	if (!item->adaptor.pinup) {
		ErrPrint("symbol: livebox_pinup - %s\n", dlerror());
	}

	item->adaptor.is_updated = (adaptor_is_updated_t)dlsym(item->handle, "livebox_need_to_update");
	if (!item->adaptor.is_updated) {
		ErrPrint("symbol: livebox_need_to_update - %s\n", dlerror());
	}

	item->adaptor.update_content = (adaptor_update_content_t)dlsym(item->handle, "livebox_update_content");
	if (!item->adaptor.update_content) {
		ErrPrint("symbol: livebox_update_content - %s\n", dlerror());
	}

	item->adaptor.clicked = (adaptor_clicked_t)dlsym(item->handle, "livebox_clicked");
	if (!item->adaptor.clicked) {
		ErrPrint("symbol: livebox_clicked - %s\n", dlerror());
	}

	item->adaptor.script_event = (adaptor_script_t)dlsym(item->handle, "livebox_content_event");
	if (!item->adaptor.script_event) {
		ErrPrint("symbol: livebox_content_event - %s\n", dlerror());
	}

	item->adaptor.resize = (adaptor_resize_t)dlsym(item->handle, "livebox_resize");
	if (!item->adaptor.resize) {
		ErrPrint("symbol: livebox_resize - %s\n", dlerror());
	}

	item->adaptor.create_needed = (adaptor_create_needed_t)dlsym(item->handle, "livebox_need_to_create");
	if (!item->adaptor.create_needed) {
		ErrPrint("symbol: livebox_need_to_create - %s\n", dlerror());
	}

	item->adaptor.change_group = (adaptor_change_group_t)dlsym(item->handle, "livebox_change_group");
	if (!item->adaptor.change_group) {
		ErrPrint("symbol: livebox_change_group - %s\n", dlerror());
	}

	item->adaptor.get_output_info = (adaptor_get_output_info_t)dlsym(item->handle, "livebox_get_info");
	if (!item->adaptor.get_output_info) {
		ErrPrint("symbol: livebox_get_info - %s\n", dlerror());
	}

	item->adaptor.initialize = (adaptor_initialize_t)dlsym(item->handle, "livebox_initialize");
	if (!item->adaptor.initialize) {
		ErrPrint("symbol: livebox_initialize - %s\n", dlerror());
	}

	item->adaptor.finalize = (adaptor_finalize_t)dlsym(item->handle, "livebox_finalize");
	if (!item->adaptor.finalize) {
		ErrPrint("symbol: livebox_finalize - %s\n", dlerror());
	}

	item->adaptor.need_to_destroy = (adaptor_need_to_destroy_t)dlsym(item->handle, "livebox_need_to_destroy");
	if (!item->adaptor.need_to_destroy) {
		ErrPrint("symbol: livebox_need_to_destroy - %s\n", dlerror());
	}

	item->adaptor.sys_event = (adaptor_system_event_t)dlsym(item->handle, "livebox_system_event");
	if (!item->adaptor.sys_event) {
		ErrPrint("symbol: lievbox_system_event - %s\n", dlerror());
	}

	item->adaptor.is_pinned_up = (adaptor_is_pinned_up_t)dlsym(item->handle, "livebox_is_pinned_up");
	if (!item->adaptor.is_pinned_up) {
		ErrPrint("symbol: livebox_is_pinned_up - %s\n", dlerror());
	}

	if (item->adaptor.initialize) {
		int ret;
		fault_mark_call(pkgname, "initialize", __func__, USE_ALARM, DEFAULT_LIFE_TIMER);
		ret = item->adaptor.initialize(pkgname);
		fault_unmark_call(pkgname, "initialize", __func__, USE_ALARM);
		if (ret < 0) {
			ErrPrint("Failed to initialize package %s\n", pkgname);
			main_heap_monitor_del_target(item->so_fname);
			delete_livebox(item);
			return NULL;
		}
	}

	s_info.livebox_list = eina_list_append(s_info.livebox_list, item);
	return item;
}

static struct so_item *new_livebox(const char *pkgname)
{
	struct so_item *item;
	char *errmsg;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Memory: %s\n", strerror(errno));
		return NULL;
	}

	item->pkgname = strdup(pkgname);
	if (!item->pkgname) {
		ErrPrint("Memory: %s\n", strerror(errno));
		free(item);
		return NULL;
	}

	/*! \TODO:
	 * item->timeout
	 */

	/*! \TODO
	 * item->has_livbox_script
	 */

	item->inst_list = NULL;

	item->so_fname = so_path_alloc(pkgname);
	if (!item->so_fname) {
		free(item->pkgname);
		free(item);
		return NULL;
	}

	fault_mark_call(pkgname, __func__, __func__, USE_ALARM, DEFAULT_LOAD_TIMER);
	item->handle = dlopen(item->so_fname, RTLD_LOCAL | RTLD_NOW | RTLD_DEEPBIND);
	if (!item->handle) {
		fault_unmark_call(pkgname, __func__, __func__, USE_ALARM);
		ErrPrint("dlopen: %s - %s\n", dlerror(), item->so_fname);
		free(item->so_fname);
		free(item->pkgname);
		free(item);
		return NULL;
	}
	fault_unmark_call(pkgname, __func__, __func__, USE_ALARM);

	errmsg = dlerror();
	if (errmsg) {
		DbgPrint("dlerror(can be ignored): %s\n", errmsg);
	}

	item->livebox.create = (create_t)dlsym(item->handle, "livebox_create");
	if (!item->livebox.create) {
		ErrPrint("symbol: livebox_create - %s\n", dlerror());
		delete_livebox(item);
		return NULL;
	}

	item->livebox.destroy = (destroy_t)dlsym(item->handle, "livebox_destroy");
	if (!item->livebox.destroy) {
		ErrPrint("symbol: livebox_destroy - %s\n", dlerror());
		delete_livebox(item);
		return NULL;
	}

	item->livebox.pinup = (pinup_t)dlsym(item->handle, "livebox_pinup");
	if (!item->livebox.pinup) {
		ErrPrint("symbol: livebox_pinup - %s\n", dlerror());
	}

	item->livebox.is_updated = (is_updated_t)dlsym(item->handle, "livebox_need_to_update");
	if (!item->livebox.is_updated) {
		ErrPrint("symbol: livebox_need_to_update - %s\n", dlerror());
	}

	item->livebox.update_content = (update_content_t)dlsym(item->handle, "livebox_update_content");
	if (!item->livebox.update_content) {
		ErrPrint("symbol: livebox_update_content - %s\n", dlerror());
	}

	item->livebox.clicked = (clicked_t)dlsym(item->handle, "livebox_clicked");
	if (!item->livebox.clicked) {
		ErrPrint("symbol: livebox_clicked - %s\n", dlerror());
	}

	item->livebox.script_event = (script_t)dlsym(item->handle, "livebox_content_event");
	if (!item->livebox.script_event) {
		ErrPrint("symbol: livebox_content_event - %s\n", dlerror());
	}

	item->livebox.resize = (resize_t)dlsym(item->handle, "livebox_resize");
	if (!item->livebox.resize) {
		ErrPrint("symbol: livebox_resize - %s\n", dlerror());
	}

	item->livebox.create_needed = (create_needed_t)dlsym(item->handle, "livebox_need_to_create");
	if (!item->livebox.create_needed) {
		ErrPrint("symbol: livebox_need_to_create - %s\n", dlerror());
	}

	item->livebox.change_group = (change_group_t)dlsym(item->handle, "livebox_change_group");
	if (!item->livebox.change_group) {
		ErrPrint("symbol: livebox_change_group - %s\n", dlerror());
	}

	item->livebox.get_output_info = (get_output_info_t)dlsym(item->handle, "livebox_get_info");
	if (!item->livebox.get_output_info) {
		ErrPrint("symbol: livebox_get_info - %s\n", dlerror());
	}

	item->livebox.initialize = (initialize_t)dlsym(item->handle, "livebox_initialize");
	if (!item->livebox.initialize) {
		ErrPrint("symbol: livebox_initialize - %s\n", dlerror());
	}

	item->livebox.finalize = (finalize_t)dlsym(item->handle, "livebox_finalize");
	if (!item->livebox.finalize) {
		ErrPrint("symbol: livebox_finalize - %s\n", dlerror());
	}

	item->livebox.need_to_destroy = (need_to_destroy_t)dlsym(item->handle, "livebox_need_to_destroy");
	if (!item->livebox.need_to_destroy) {
		ErrPrint("symbol: livebox_need_to_destroy - %s\n", dlerror());
	}

	item->livebox.sys_event = (system_event_t)dlsym(item->handle, "livebox_system_event");
	if (!item->livebox.sys_event) {
		ErrPrint("symbol: livebox_system_event - %s\n", dlerror());
	}

	item->livebox.is_pinned_up = (is_pinned_up_t)dlsym(item->handle, "livebox_is_pinned_up");
	if (!item->livebox.is_pinned_up) {
		ErrPrint("symbol: livebox_is_pinned_up - %s\n", dlerror());
	}

	main_heap_monitor_add_target(item->so_fname);

	if (item->livebox.initialize) {
		int ret;
		fault_mark_call(pkgname, "initialize", __func__, USE_ALARM, DEFAULT_LIFE_TIMER);
		ret = item->livebox.initialize(pkgname);
		fault_unmark_call(pkgname, "initialize", __func__, USE_ALARM);
		if (ret < 0) {
			ErrPrint("Failed to initialize package %s\n", pkgname);
			main_heap_monitor_del_target(item->so_fname);
			delete_livebox(item);
			return NULL;
		}
	}

	s_info.livebox_list = eina_list_append(s_info.livebox_list, item);
	return item;
}

static inline struct instance *new_instance(const char *id, const char *content, const char *cluster, const char *category)
{
	struct instance *inst;

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	inst->id = strdup(id);
	if (!inst->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst);
		return NULL;
	}

	DbgPrint("Default content: [%s]\n", content);
	if (content) {
		inst->content = strdup(content);
		if (!inst->content) {
			ErrPrint("memory: %s\n", strerror(errno));
			free(inst->id);
			free(inst);
			return NULL;
		}
	}

	if (cluster) {
		inst->cluster = strdup(cluster);
		if (!inst->cluster) {
			ErrPrint("memory: %s\n", strerror(errno));
			free(inst->id);
			free(inst->content);
			free(inst);
			return NULL;
		}
	}

	if (category) {
		inst->category = strdup(category);
		if (!inst->category) {
			ErrPrint("memory: %s\n", strerror(errno));
			free(inst->cluster);
			free(inst->id);
			free(inst->content);
			free(inst);
			return NULL;
		}
	}

	return inst;
}

static inline int delete_instance(struct instance *inst)
{
	free(inst->cluster);
	free(inst->category);
	free(inst->id);
	free(inst->content);
	free(inst->title);
	free(inst);
	return LB_STATUS_SUCCESS;
}

static inline struct instance *find_instance(struct so_item *item, const char *id)
{
	struct instance *inst;
	Eina_List *l;

	EINA_LIST_FOREACH(item->inst_list, l, inst) {
		if (!strcmp(inst->id, id)) {
			return inst;
		}
	}

	return NULL;
}

HAPI struct instance *so_find_instance(const char *pkgname, const char *id)
{
	struct so_item *item;

	item = find_livebox(pkgname);
	if (!item) {
		return NULL;
	}

	return find_instance(item, id);
}

HAPI int so_create(const char *pkgname, const char *id, const char *content_info, int timeout, int has_livebox_script, const char *cluster, const char *category, const char *abi, struct instance **out)
{
	struct so_item *item;
	struct instance *inst;
	int ret;

	item = find_livebox(pkgname);
	if (item) {
		inst = find_instance(item, id);
		if (inst) {
			ErrPrint("Instance: %s - %s is already exists\n", pkgname, id);
			return LB_STATUS_ERROR_EXIST;
		}
	} else {
		if (!strcasecmp(abi, "c")) {
			item = new_livebox(pkgname);
		} else {
			item = new_adaptor(pkgname, abi);
		}

		if (!item) {
			return LB_STATUS_ERROR_FAULT;
		}
	}

	inst = new_instance(id, content_info, cluster, category);
	if (!inst) {
		if (!item->inst_list) {
			delete_livebox(item);
		}

		return LB_STATUS_ERROR_FAULT;
	}

	item->inst_list = eina_list_append(item->inst_list, inst);
	item->has_livebox_script = has_livebox_script;
	item->timeout = timeout;

	fault_mark_call(pkgname, id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_CREATE;
	if (item->adaptor.create) {
		ret = item->adaptor.create(pkgname, util_uri_to_path(id), content_info, cluster, category);
	} else if (item->livebox.create) {
		ret = item->livebox.create(util_uri_to_path(id), content_info, cluster, category);
	} else { /*! \NOTE: This is not possible, but for the exceptional handling */
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(pkgname, id, __func__, USE_ALARM);

	if (ret < 0) {
		item->inst_list = eina_list_remove(item->inst_list, inst);
		delete_instance(inst);

		if (!item->inst_list) {
			/* There is no instances, unload this livebox */
			s_info.livebox_list = eina_list_remove(s_info.livebox_list, item);
			delete_livebox(item);
		}
		return ret;
	}

	inst->item = item;
	*out = inst;
	return ret;
}

HAPI int so_destroy(struct instance *inst)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_DESTROY;
	if (item->adaptor.destroy) {
		ret = item->adaptor.destroy(item->pkgname, util_uri_to_path(inst->id));
	} else if (item->livebox.destroy) {
		ret = item->livebox.destroy(util_uri_to_path(inst->id));
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	item->inst_list = eina_list_remove(item->inst_list, inst);
	delete_instance(inst);

	if (!item->inst_list) {
		s_info.livebox_list = eina_list_remove(s_info.livebox_list, item);
		delete_livebox(item);
	}

	return ret;
}

HAPI char *so_pinup(struct instance *inst, int pinup)
{
	struct so_item *item;
	char *ret;

	item = inst->item;
	if (!item) {
		return NULL;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_PINUP;
	if (item->adaptor.pinup) {
		ret = item->adaptor.pinup(item->pkgname, util_uri_to_path(inst->id), pinup);
	} else if (item->livebox.pinup) {
		ret = item->livebox.pinup(util_uri_to_path(inst->id), pinup);
	} else {
		ret = NULL;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;
	
	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	return ret;
}

HAPI int so_is_pinned_up(struct instance *inst)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_IS_PINNED_UP;
	if (item->adaptor.is_pinned_up) {
		ret = item->adaptor.is_pinned_up(item->pkgname, util_uri_to_path(inst->id));
	} else if (item->livebox.is_pinned_up) {
		ret = item->livebox.is_pinned_up(util_uri_to_path(inst->id));
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	return ret;
}

HAPI int so_is_updated(struct instance *inst)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_NEED_TO_UPDATE;
	if (item->adaptor.is_updated) {
		ret = item->adaptor.is_updated(item->pkgname, util_uri_to_path(inst->id));
	} else if (item->livebox.is_updated) {
		ret = item->livebox.is_updated(util_uri_to_path(inst->id));
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	return ret;
}

HAPI int so_need_to_destroy(struct instance *inst)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_NEED_TO_DESTROY;
	if (item->adaptor.need_to_destroy) {
		ret = item->adaptor.need_to_destroy(item->pkgname, util_uri_to_path(inst->id));
	} else if (item->livebox.need_to_destroy) {
		ret = item->livebox.need_to_destroy(util_uri_to_path(inst->id));
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	return ret;
}

HAPI int so_update(struct instance *inst)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_UPDATE_CONTENT;
	if (item->adaptor.update_content) {
		ret = item->adaptor.update_content(item->pkgname, util_uri_to_path(inst->id));
	} else if (item->livebox.update_content) {
		ret = item->livebox.update_content(util_uri_to_path(inst->id));
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	return ret;
}

HAPI int so_clicked(struct instance *inst, const char *event, double timestamp, double x, double y)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	DbgPrint("PERF_DBOX\n");
	s_info.current_op = LIVEBOX_OP_CLICKED;
	if (item->adaptor.clicked) {
		ret = item->adaptor.clicked(item->pkgname, util_uri_to_path(inst->id), event, timestamp, x, y);
	} else if (item->livebox.clicked) {
		ret = item->livebox.clicked(util_uri_to_path(inst->id), event, timestamp, x, y);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	return ret;
}

HAPI int so_script_event(struct instance *inst, const char *emission, const char *source, struct event_info *event_info)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_CONTENT_EVENT;
	if (item->adaptor.script_event) {
		ret = item->adaptor.script_event(item->pkgname, util_uri_to_path(inst->id), emission, source, event_info);
	} else if (item->livebox.script_event) {
		ret = item->livebox.script_event(util_uri_to_path(inst->id), emission, source, event_info);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	return ret;
}

HAPI int so_resize(struct instance *inst, int w, int h)
{
	struct so_item *item;
	int ret;
	int type;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	type = livebox_service_size_type(w, h);
	if (type == LB_SIZE_TYPE_UNKNOWN) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_RESIZE;
	if (item->adaptor.resize) {
		ret = item->adaptor.resize(item->pkgname, util_uri_to_path(inst->id), type);
	} else if (item->livebox.resize) {
		ret = item->livebox.resize(util_uri_to_path(inst->id), type);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);

	return ret;
}

HAPI int so_create_needed(const char *pkgname, const char *cluster, const char *category, const char *abi)
{
	struct so_item *item;
	int ret;

	item = find_livebox(pkgname);
	if (!item) {
		if (!strcasecmp(abi, "c")) {
			item = new_livebox(pkgname);
		} else {
			item = new_adaptor(pkgname, abi);
		}

		if (!item) {
			return LB_STATUS_ERROR_FAULT;
		}
	}

	fault_mark_call(item->pkgname, __func__, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_NEED_TO_CREATE;
	if (item->adaptor.create_needed) {
		ret = item->adaptor.create_needed(pkgname, cluster, category);
	} else if (item->livebox.create_needed) {
		ret = item->livebox.create_needed(cluster, category);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, __func__, __func__, USE_ALARM);

	DbgPrint("[%s] returns %d\n", pkgname, ret);
	return ret;
}

HAPI int so_change_group(struct instance *inst, const char *cluster, const char *category)
{
	struct so_item *item;
	int ret;
	char *tmp_cluster;
	char *tmp_category;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	tmp_cluster = strdup(cluster);
	if (!tmp_cluster) {
		return LB_STATUS_ERROR_MEMORY;
	}

	tmp_category = strdup(category);
	if (!tmp_category) {
		free(tmp_cluster);
		return LB_STATUS_ERROR_MEMORY;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_CHANGE_GROUP;
	if (item->adaptor.change_group) {
		ret = item->adaptor.change_group(item->pkgname, util_uri_to_path(inst->id), cluster, category);
	} else if (item->livebox.change_group) {
		ret = item->livebox.change_group(util_uri_to_path(inst->id), cluster, category);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	if (ret >= 0) {
		free(inst->cluster);
		free(inst->category);

		inst->cluster = tmp_cluster;
		inst->category = tmp_category;
	} else {
		free(tmp_cluster);
		free(tmp_category);
	}

	return ret;
}

HAPI int so_get_output_info(struct instance *inst, int *w, int *h, double *priority, char **content, char **title)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	*content = NULL;
	*title = NULL;

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_GET_INFO;
	if (item->adaptor.get_output_info) {
		ret = item->adaptor.get_output_info(item->pkgname, util_uri_to_path(inst->id), w, h, priority, content, title);
	} else if (item->livebox.get_output_info) {
		ret = item->livebox.get_output_info(util_uri_to_path(inst->id), w, h, priority, content, title);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	if (ret >= 0) {
		inst->w = *w;
		inst->h = *h;
		inst->priority = *priority;

		/*!
		 * \todo
		 * Add "*content" "*title" address validation code.
		 * using mcheck?
		 */

		if (*content) {
			free(inst->content);
			inst->content = *content;
		}

		if (*title) {
			free(inst->title);
			inst->title = *title;
		}
	}

	if (main_heap_monitor_is_enabled()) {
		DbgPrint("%s allocates %d bytes\n", item->pkgname, main_heap_monitor_target_usage(item->so_fname));
	}

	return ret;
}

HAPI int so_sys_event(struct instance *inst, int event)
{
	struct so_item *item;
	int ret;

	item = inst->item;
	if (!item) {
		return LB_STATUS_ERROR_INVALID;
	}

	fault_mark_call(item->pkgname, inst->id, __func__, USE_ALARM, DEFAULT_LIFE_TIMER);

	s_info.current_op = LIVEBOX_OP_SYSTEM_EVENT;
	if (item->adaptor.sys_event) {
		ret = item->adaptor.sys_event(item->pkgname, util_uri_to_path(inst->id), event);
	} else if (item->livebox.sys_event) {
		ret = item->livebox.sys_event(util_uri_to_path(inst->id), event);
	} else {
		ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;
	}
	s_info.current_op = LIVEBOX_OP_UNKNOWN;

	fault_unmark_call(item->pkgname, inst->id, __func__, USE_ALARM);
	return ret;
}

HAPI enum current_operations so_current_op(void)
{
	return s_info.current_op;
}

/* End of a file */
