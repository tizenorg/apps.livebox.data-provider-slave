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

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <pthread.h>

#include <dlog.h>
#include <aul.h>

#include <Eina.h>
#include <Ecore.h>

#include "critical_log.h"
#include "util.h"
#include "conf.h"
#include "debug.h"

HAPI int util_check_ext(const char *icon, const char *ext)
{
	int len;

	len = strlen(icon) - 1;
	while (len >= 0 && *ext && icon[len] == *ext) {
		len--;
		ext++;
	}

	return *ext ? 0 : 1;
}

HAPI double util_timestamp(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
}

HAPI const char *util_basename(const char *name)
{
	int length;

	length = name ? strlen(name) : 0;
	if (!length)
		return ".";

	while (--length > 0 && name[length] != '/');

	return length <= 0 ? name : name + length + (name[length] == '/');
}

/*!
 * \note
 * Just trying to find the nearest module.
 * It could be wrong.
 */
HAPI char *util_get_current_module(char **symbol)
{
	int *stack;
	Dl_info dinfo;
	const char *ptr;
	char *ret;
	pthread_attr_t attr;
	unsigned int stack_boundary = 0;
	unsigned int stack_size = 0;
	register int i;

	if (!pthread_getattr_np(pthread_self(), &attr)) {
		if (!pthread_attr_getstack(&attr, (void *)&stack_boundary, &stack_size))
			stack_boundary += stack_size;
		pthread_attr_destroy(&attr);
	}

	ret = NULL;
	for (i = 0, stack = (int *)&stack; (unsigned int)stack < stack_boundary ; stack++, i++) {
		if (!dladdr((void *)*stack, &dinfo))
			continue;

		ptr = util_basename(dinfo.dli_fname);
		if (strncmp(ptr, "liblive-", strlen("liblive-"))) {
			DbgPrint("[%d] fname[%s] symbol[%s]\n", i, dinfo.dli_fname, dinfo.dli_sname);
			/*
			if (!strcmp(ptr, EXEC_NAME))
				return EXEC_NAME;
			*/

			continue;
		}

		ret = strdup(ptr);

		if (symbol) {
			if (dinfo.dli_sname)
				*symbol = strdup(dinfo.dli_sname);
			else
				*symbol = NULL;
		}
		break;
	}

	return ret;
}

HAPI const char *util_uri_to_path(const char *uri)
{
	int len;

	len = strlen(SCHEMA_FILE);
	if (strncasecmp(uri, SCHEMA_FILE, len))
		return NULL;

	return uri + len;
}

static inline void compensate_timer(Ecore_Timer *timer)
{
	struct timeval tv;
	struct timeval compensator;
	double delay;
	double pending;

	if (ecore_timer_interval_get(timer) <= 1.0f) {
		DbgPrint("Doesn't need to sync the timer to start from ZERO sec\n");
		return;
	}

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return;
	}

	compensator.tv_sec = tv.tv_sec % 60;
	if (compensator.tv_sec == 0)
		compensator.tv_sec = 59;

	delay = 60.0f - ((double)compensator.tv_sec + ((double)tv.tv_usec / 1000000.0f));
	pending = ecore_timer_pending_get(timer);
	ecore_timer_delay(timer, delay - pending);
	DbgPrint("COMPENSATED: %lf\n", delay);
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer)
		return NULL;

	compensate_timer(timer);
	return timer;
}

HAPI void util_timer_interval_set(void *timer, double interval)
{
	ecore_timer_interval_set(timer, interval);
	compensate_timer(timer);
}

/* End of a file */
