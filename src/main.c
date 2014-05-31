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
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <mcheck.h>
#include <dlfcn.h>

#include <Elementary.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <Ecore.h>
#include <Ecore_X.h>
#include <app.h>
#include <Edje.h>
#include <Eina.h>

#include <system_settings.h>

#include <dlog.h>
#include <bundle.h>
#include <livebox-service.h>
#include <provider.h>
#include <vconf.h>
#include <livebox.h>

#include "critical_log.h"
#include "debug.h"
#include "conf.h"
#include "fault.h"
#include "update_monitor.h"
#include "client.h"
#include "util.h"
#include "so_handler.h"
#include "lb.h"

#define TEXT_CLASS	"tizen"
#define DEFAULT_FONT_SIZE	-100

#if !defined(VCONFKEY_SETAPPL_ACCESSIBILITY_FONT_NAME)
#define VCONFKEY_SETAPPL_ACCESSIBILITY_FONT_NAME "db/setting/accessibility/font_name"
#endif

#if !defined(VCONFKEY_SETAPPL_ACCESSIBILITY_FONT_SIZE)
#define VCONFKEY_SETAPPL_ACCESSIBILITY_FONT_SIZE "db/setting/accessibility/font_size"
#endif

static struct info {
	char *font_name;
	int font_size;
	int (*heap_monitor_initialized)(void);
	size_t (*heap_monitor_target_usage)(const char *name);
	int (*heap_monitor_add_target)(const char *name);
	int (*heap_monitor_del_target)(const char *name);
	void *heap_monitor;
} s_info = {
	.font_name = NULL,
	.font_size = DEFAULT_FONT_SIZE,
	.heap_monitor_initialized = NULL,
	.heap_monitor_target_usage = NULL,
	.heap_monitor_add_target = NULL,
	.heap_monitor_del_target = NULL,
	.heap_monitor = NULL,
};

static void update_font_cb(void *data)
{
	Eina_List *list;
	char *text;

	list = edje_text_class_list();
	DbgPrint("List: %p\n", list);
	if (list) {
		EINA_LIST_FREE(list, text) {
			if (!strncasecmp(text, TEXT_CLASS, strlen(TEXT_CLASS))) {
				DbgPrint("Update text class %s (%s, %d)\n", text, s_info.font_name, DEFAULT_FONT_SIZE);
				edje_text_class_del(text);
				edje_text_class_set(text, s_info.font_name, DEFAULT_FONT_SIZE);
			} else {
				DbgPrint("Skip text class %s\n", text);
			}
		}
	} else {
		DbgPrint("New (%s, %d)\n", s_info.font_name, DEFAULT_FONT_SIZE);
		edje_text_class_set(TEXT_CLASS, s_info.font_name, DEFAULT_FONT_SIZE);
	}

	DbgPrint("Call system event\n");
	lb_system_event_all(LB_SYS_EVENT_FONT_CHANGED);
}

static void font_changed_cb(keynode_t *node, void *user_data)
{
	char *font_name;

#if defined(WEARABLE)
	evas_font_reinit();
#endif

	if (s_info.font_name) {
		font_name = vconf_get_str("db/setting/accessibility/font_name");
		if (!font_name) {
			ErrPrint("Invalid font name (NULL)\n");
			return;
		}

		if (!strcmp(s_info.font_name, font_name)) {
			DbgPrint("Font is not changed (Old: %s(%p) <> New: %s(%p))\n", s_info.font_name, s_info.font_name, font_name, font_name);
			free(font_name);
			return;
		}

		DbgPrint("Release old font name: %s(%p)\n", s_info.font_name, s_info.font_name);
		free(s_info.font_name);
	} else {
		int ret;

		font_name = NULL;
		ret = system_settings_get_value_string(SYSTEM_SETTINGS_KEY_FONT_TYPE, &font_name);
		if (ret != SYSTEM_SETTINGS_ERROR_NONE || !font_name) {
			ErrPrint("System settings: %d, font_name[%p]\n", ret, font_name);
			return;
		}
	}

	s_info.font_name = font_name;
	DbgPrint("Font name is changed to %s(%p)\n", s_info.font_name, s_info.font_name);

	/*!
	 * \NOTE
	 * Try to update all liveboxes
	 */
	update_font_cb(NULL);
}

static inline int convert_font_size(int size)
{
	switch (size) {
	case SYSTEM_SETTINGS_FONT_SIZE_SMALL:
		size = -80;
		break;
	case SYSTEM_SETTINGS_FONT_SIZE_NORMAL:
		size = -100;
		break;
	case SYSTEM_SETTINGS_FONT_SIZE_LARGE:
		size = -150;
		break;
	case SYSTEM_SETTINGS_FONT_SIZE_HUGE:
		size = -190;
		break;
	case SYSTEM_SETTINGS_FONT_SIZE_GIANT:
		size = -250;
		break;
	default:
		size = -100;
		break;
	}

	DbgPrint("Return size: %d\n", size);
	return size;
}

static void font_size_cb(system_settings_key_e key, void *user_data)
{
	int size;

	if (system_settings_get_value_int(SYSTEM_SETTINGS_KEY_FONT_SIZE, &size) != SYSTEM_SETTINGS_ERROR_NONE) {
		return;
	}

	size = convert_font_size(size);

	if (size == s_info.font_size) {
		DbgPrint("Font size is not changed\n");
		return;
	}

	s_info.font_size = size;
	DbgPrint("Font size is changed to %d, but don't try to update it.\n", size);
}

static void mmc_changed_cb(keynode_t *node, void *user_data)
{
	DbgPrint("MMC status is changed\n");
	lb_system_event_all(LB_SYS_EVENT_MMC_STATUS_CHANGED);
}

static void time_changed_cb(keynode_t *node, void *user_data)
{
	if (vconf_keynode_get_int(node) != VCONFKEY_SYSMAN_STIME_CHANGED) {
		return;
	}

	DbgPrint("Time is changed\n");
	lb_system_event_all(LB_SYS_EVENT_TIME_CHANGED);
}

static void initialize_glib_type_system(void)
{
	GType type;

	type = G_TYPE_DBUS_ACTION_GROUP;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_ANNOTATION_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_ARG_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_AUTH_OBSERVER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_CALL_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_CAPABILITY_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_CONNECTION;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_CONNECTION_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_ERROR;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_INTERFACE;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_INTERFACE_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_INTERFACE_SKELETON;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_INTERFACE_SKELETON_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MENU_MODEL;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MESSAGE;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MESSAGE_BYTE_ORDER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MESSAGE_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MESSAGE_HEADER_FIELD;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_MESSAGE_TYPE;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_METHOD_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_METHOD_INVOCATION;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_NODE_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_MANAGER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_MANAGER_CLIENT;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_MANAGER_CLIENT_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_MANAGER_SERVER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_PROXY;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_OBJECT_SKELETON;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_PROPERTY_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_PROPERTY_INFO_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_PROXY;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_PROXY_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SEND_MESSAGE_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SERVER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SERVER_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SIGNAL_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SIGNAL_INFO;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = G_TYPE_DBUS_SUBTREE_FLAGS;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_NODE;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_OBJECT;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_ARRAY;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_ARRAY;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_SERIALIZABLE;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_PARSER;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
	type = JSON_TYPE_GENERATOR;
	if (type != G_TYPE_OBJECT) {
		DbgPrint("initialized\n");
	}
}

static bool app_create(void *argv)
{
	int ret;

	/*!
	 * Touch the glib type system
	 */
	initialize_glib_type_system();

	conf_update_size();

	DbgPrint("Scale factor: %lf\n", elm_config_scale_get());

	if (COM_CORE_THREAD) {
		if (setenv("PROVIDER_COM_CORE_THREAD", "true", 0) < 0) {
			ErrPrint("setenv: %s\n", strerror(errno));
		}
	} else {
		if (setenv("PROVIDER_COM_CORE_THREAD", "false", 0) < 0){
			ErrPrint("setenv: %s\n", strerror(errno));
		}
	}

	ret = livebox_service_init();
	if (ret < 0) {
		DbgPrint("Livebox service init: %d\n", ret);
	}

	/*
	 * \note
	 * Slave is not able to initiate system, before
	 * receive its name from the master
	 *
	 * So create callback doesn't do anything.
	 */
	ret = fault_init(argv);
	if (ret < 0) {
		DbgPrint("Crash recover is initiated: %d\n", ret);
	}

	ret = update_monitor_init();
	if (ret < 0) {
		DbgPrint("Content update monitor is initiated: %d\n", ret);
	}

	ret = vconf_notify_key_changed("db/setting/accessibility/font_name", font_changed_cb, NULL);
	if (ret < 0) {
		DbgPrint("System font changed callback is added: %d\n", ret);
	}
	
	ret = system_settings_set_changed_cb(SYSTEM_SETTINGS_KEY_FONT_SIZE, font_size_cb, NULL);
	if (ret < 0) {
		DbgPrint("System font size changed callback is added: %d\n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_SYSMAN_STIME, time_changed_cb, NULL);
	if (ret < 0) {
		DbgPrint("System time changed event callback added: %d\n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_SYSMAN_MMC_STATUS, mmc_changed_cb, NULL);
	if (ret < 0) {
		DbgPrint("MMC status changed event callback added: %d\n", ret);
	}

	font_changed_cb(NULL, NULL);
	font_size_cb(SYSTEM_SETTINGS_KEY_FONT_SIZE, NULL);

	return TRUE;
}

static void app_terminate(void *data)
{
	int ret;

	DbgPrint("Terminating provider\n");

	ret = lb_delete_all_deleteme();
	if (ret < 0) {
		DbgPrint("Delete all deleteme: %d\n", ret);
	}

	ret = lb_delete_all();
	if (ret < 0) {
		DbgPrint("Delete all: %d\n", ret);
	}

	ret = system_settings_unset_changed_cb(SYSTEM_SETTINGS_KEY_FONT_SIZE);
	if (ret < 0) {
		DbgPrint("unset fontsize: %d\n", ret);
	}

	ret = vconf_ignore_key_changed("db/setting/accessibility/font_name", font_changed_cb);
	if (ret < 0) {
		DbgPrint("Remove font change callback: %d\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_SYSMAN_STIME, time_changed_cb);
	if (ret < 0) {
		DbgPrint("Remove time changed callback: %d\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_SYSMAN_MMC_STATUS, mmc_changed_cb);
	if (ret < 0) {
		DbgPrint("Remove MMC status changed callback: %d\n", ret);
	}

	ret = update_monitor_fini();
	if (ret < 0) {
		DbgPrint("Content update monitor is finalized: %d\n", ret);
	}

	ret = fault_fini();
	if (ret < 0) {
		DbgPrint("Crash recover is finalized: %d\n", ret);
	}

	ret = client_fini();
	if (ret < 0) {
		DbgPrint("client finalized: %d\n", ret); 
	}

	ret = livebox_service_fini();
	if (ret < 0) {
		DbgPrint("livebox service fini: %d\n", ret);
	}

	free(s_info.font_name);
	s_info.font_name = NULL;
	return;
}

static void app_pause(void *data)
{
	/* Will not be invoked */
	return;
}

static void app_resume(void *data)
{
	/* Will not be invoked */
	return;
}

static void app_region_changed(void *data)
{
	lb_system_event_all(LB_SYS_EVENT_REGION_CHANGED);
}

static void app_language_changed(void *data)
{
	lb_system_event_all(LB_SYS_EVENT_LANG_CHANGED);
}

static void app_service(service_h service, void *data)
{
	int ret;
	char *name;
	char *secured;
	static int initialized = 0;

	if (initialized) {
		ErrPrint("Already initialized\n");
		return;
	}

	ret = service_get_extra_data(service, "name", &name);
	if (ret != SERVICE_ERROR_NONE) {
		ErrPrint("Name is not valid\n");
		return;
	}

	ret = service_get_extra_data(service, "secured", &secured);
	if (ret != SERVICE_ERROR_NONE) {
		free(name);
		ErrPrint("Secured is not valid\n");
		return;
	}

	if (!strcasecmp(secured, "true")) {
		/* Don't use the update timer */
		lb_turn_secured_on();
	}

	DbgPrint("Name assigned: %s\n", name);
	DbgPrint("Secured: %s\n", secured);
	ret = client_init(name);
	free(name);
	free(secured);

	initialized = 1;
	return;
}

#if defined(_ENABLE_MCHECK)
static inline void mcheck_cb(enum mcheck_status status)
{
	char *ptr;

	ptr = util_get_current_module(NULL);

	switch (status) {
	case MCHECK_DISABLED:
		ErrPrint("[DISABLED] Heap incosistency detected: %s\n", ptr);
		break;
	case MCHECK_OK:
		ErrPrint("[OK] Heap incosistency detected: %s\n", ptr);
		break;
	case MCHECK_HEAD:
		ErrPrint("[HEAD] Heap incosistency detected: %s\n", ptr);
		break;
	case MCHECK_TAIL:
		ErrPrint("[TAIL] Heap incosistency detected: %s\n", ptr);
		break;
	case MCHECK_FREE:
		ErrPrint("[FREE] Heap incosistency detected: %s\n", ptr);
		break;
	default:
		break;
	}
}
#endif

#define BIN_PATH "/usr/apps/org.tizen.data-provider-slave/bin/"
int main(int argc, char *argv[])
{
	int ret;
	app_event_callback_s event_callback;
	const char *option;

	memset(argv[0], 0, strlen(argv[0]));
	strcpy(argv[0], BIN_PATH "data-provider-slave");
	DbgPrint("Replace argv[0] with %s\n", argv[0]);

#if defined(_ENABLE_MCHECK)
	mcheck(mcheck_cb);
#endif
	option = getenv("PROVIDER_DISABLE_CALL_OPTION");
	if (option && !strcasecmp(option, "true")) {
		fault_disable_call_option();
	}

	option = getenv("PROVIDER_HEAP_MONITOR_START");
	if (option && !strcasecmp(option, "true")) {
		s_info.heap_monitor = dlopen("/usr/lib/libheap-monitor.so", RTLD_NOW);
		if (s_info.heap_monitor) {
			s_info.heap_monitor_initialized = dlsym(s_info.heap_monitor, "heap_monitor_initialized");
			s_info.heap_monitor_target_usage = dlsym(s_info.heap_monitor, "heap_monitor_target_usage");
			s_info.heap_monitor_add_target = dlsym(s_info.heap_monitor, "heap_monitor_add_target");
			s_info.heap_monitor_del_target = dlsym(s_info.heap_monitor, "heap_monitor_del_target");
		}
	}

	setenv("BUFMGR_LOCK_TYPE", "once", 0);
	setenv("BUFMGR_MAP_CACHE", "true", 0);

	ret = conf_loader();
	if (ret < 0) {
		DbgPrint("Configureation manager is initiated: %d\n", ret);
	}

	critical_log_init(util_basename(argv[0]));

	event_callback.create = app_create;
	event_callback.terminate = app_terminate;
	event_callback.pause = app_pause;
	event_callback.resume = app_resume;
	event_callback.service = app_service;
	event_callback.low_memory = NULL;
	event_callback.low_battery = NULL;
	event_callback.device_orientation = NULL;
	event_callback.language_changed = app_language_changed;
	event_callback.region_format_changed = app_region_changed;
	ret = app_efl_main(&argc, &argv, &event_callback, argv);
	critical_log_fini();
	ErrPrint("Failed to init: %d\n", ret);
	if (s_info.heap_monitor) {
		if (dlclose(s_info.heap_monitor) < 0) {
			ErrPrint("dlclose: %s\n", strerror(errno));
		}
	}

	return ret;
}

HAPI int main_heap_monitor_is_enabled(void)
{
	return s_info.heap_monitor_initialized ? s_info.heap_monitor_initialized() : 0;
}

HAPI size_t main_heap_monitor_target_usage(const char *name)
{
	return s_info.heap_monitor_target_usage ? s_info.heap_monitor_target_usage(name) : 0;
}

HAPI int main_heap_monitor_add_target(const char *name)
{
	return s_info.heap_monitor_add_target ? s_info.heap_monitor_add_target(name) : 0;
}

HAPI int main_heap_monitor_del_target(const char *name)
{
	return s_info.heap_monitor_del_target ? s_info.heap_monitor_del_target(name) : 0;
}

/* End of a file */
