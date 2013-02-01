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
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <mcheck.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <Ecore.h>
#include <Ecore_X.h>
#include <app.h>
#include <Edje.h>
#include <Eina.h>

#include <dlog.h>
#include <bundle.h>
#include <heap-monitor.h>
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

/*!
 * \NOTE
 * Undocumented API
 */
extern void evas_common_font_flush(void);
extern int evas_common_font_cache_get(void);
extern void evas_common_font_cache_set(int size);

#define TEXT_CLASS	"tizen"
#define TEXT_SIZE	-100

static struct info {
	Ecore_Event_Handler *property_handler;
	int heap_monitor;
} s_info = {
	.property_handler = NULL,
	.heap_monitor = 0,
};

static Eina_Bool update_font_cb(void *data)
{
	lb_system_event_all(LB_SYS_EVENT_FONT_CHANGED);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool property_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Window_Property *info = (Ecore_X_Event_Window_Property *)event;

	if (info->atom == ecore_x_atom_get("FONT_TYPE_change") || info->atom == ecore_x_atom_get("BADA_FONT_change")) {
		Eina_List *list;
		char *text;
		char *font;
		int cache;

		font = vconf_get_str("db/setting/accessibility/font_name");
		if (!font)
			return ECORE_CALLBACK_PASS_ON;

		cache = evas_common_font_cache_get();
		evas_common_font_cache_set(0);
		evas_common_font_flush();

		list = edje_text_class_list();
		EINA_LIST_FREE(list, text) {
			if (!strncasecmp(text, TEXT_CLASS, strlen(TEXT_CLASS))) {
				edje_text_class_del(text);
				edje_text_class_set(text, font, TEXT_SIZE);
				DbgPrint("Update text class %s (%s, %d)\n", text, font, TEXT_SIZE);
			} else {
				DbgPrint("Skip text class %s\n", text);
			}
		}

		evas_common_font_cache_set(cache);
		free(font);

		/*!
		 * \NOTE
		 * Try to update all liveboxes
		 */
		if (!ecore_timer_add(0.1f, update_font_cb, NULL)) {
			ErrPrint("Failed to add timer for updating fonts\n");
			lb_system_event_all(LB_SYS_EVENT_FONT_CHANGED);
		}
	}

	return ECORE_CALLBACK_PASS_ON;
}

static void time_changed_cb(keynode_t *node, void *user_data)
{
	if (vconf_keynode_get_int(node) != VCONFKEY_SYSMAN_STIME_CHANGED)
		return;

	DbgPrint("Time is changed\n");
	lb_system_event_all(LB_SYS_EVENT_TIME_CHANGED);
}

static bool app_create(void *data)
{
	int ret;

	ret = conf_loader();
	DbgPrint("Configureation manager is initiated: %d\n", ret);

	if (COM_CORE_THREAD)
		setenv("PROVIDER_COM_CORE_THREAD", "true", 0);
	else
		setenv("PROVIDER_COM_CORE_THREAD", "false", 0);

	s_info.property_handler = ecore_event_handler_add(ECORE_X_EVENT_WINDOW_PROPERTY, property_cb, NULL);
	if (!s_info.property_handler)
		ErrPrint("Failed to add a property change event handler\n");

	ret = livebox_service_init();
	DbgPrint("Livebox service init: %d\n", ret);

	/*
	 * \note
	 * Slave is not able to initiate system, before
	 * receive its name from the master
	 *
	 * So create callback doesn't do anything.
	 */
	ret = fault_init();
	DbgPrint("Crash recover is initiated: %d\n", ret);
	ret = update_monitor_init();
	DbgPrint("Content update monitor is initiated: %d\n", ret);

	ret = vconf_notify_key_changed(VCONFKEY_SYSMAN_STIME, time_changed_cb, NULL);
	DbgPrint("System time event callback added: %d\n", ret);

	return TRUE;
}

static void app_terminate(void *data)
{
	int ret;

	ret = update_monitor_fini();
	DbgPrint("Content update monitor is finalized: %d\n", ret);
	ret = fault_fini();
	DbgPrint("Crash recover is finalized: %d\n", ret);

	ret = client_fini();
	DbgPrint("client finalized: %d\n", ret); 

	ret = livebox_service_fini();
	DbgPrint("livebox service fini: %d\n", ret);

	ecore_event_handler_del(s_info.property_handler);
	s_info.property_handler = NULL;

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

	if (!!strcasecmp(secured, "true")) {
		if (s_info.heap_monitor) {
			heap_monitor_start();
			/* Add GUARD */
			// heap_monitor_add_target("/usr/apps/org.tizen."EXEC_NAME"/bin/"EXEC_NAME);
		}
	} else {
		/* Don't use the update timer */
		lb_turn_secured_on();
	}

	DbgPrint("Name assigned: %s\n", name);
	DbgPrint("Secured: %s\n", secured);
	ret = client_init(name);
	free(name);

	initialized = 1;
	return;
}

void (*__malloc_initialize_hook)(void) = heap_monitor_init;

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

int main(int argc, char *argv[])
{
	int ret;
	app_event_callback_s event_callback;
	const char *option;

#if defined(_ENABLE_MCHECK)
	mcheck(mcheck_cb);
#endif
	option = getenv("PROVIDER_DISABLE_CALL_OPTION");
	if (option && !strcasecmp(option, "true"))
		fault_disable_call_option();

	option = getenv("PROVIDER_HEAP_MONITOR_START");
	if (option && !strcasecmp(option, "true"))
		s_info.heap_monitor = 1;

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
	ret = app_efl_main(&argc, &argv, &event_callback, NULL);
	critical_log_fini();
	return ret;
}

HAPI int main_heap_monitor_is_enabled(void)
{
	return s_info.heap_monitor;
}

/* End of a file */
