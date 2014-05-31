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

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <pthread.h>

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>
#include <aul.h>
#include <livebox-errno.h>
#include <livebox-service.h>

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

HAPI int util_get_filesize(const char *filename)
{
	struct stat buf;

	if (stat(filename, &buf) < 0) {
		ErrPrint("error: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	if (!S_ISREG(buf.st_mode)) {
		ErrPrint("%s is not a file\n", filename);
		return LB_STATUS_ERROR_INVALID;
	}

	return buf.st_size;
}

HAPI double util_timestamp(void)
{
#if defined(_USE_ECORE_TIME_GET)
	return ecore_time_get();
#else
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0) {
		static unsigned long internal_count = 0;

		ErrPrint("gettimeofday: %s\n", strerror(errno));
		tv.tv_sec = internal_count++;
		tv.tv_usec = 0;
	}

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
#endif
}

HAPI const char *util_basename(const char *name)
{
	int length;

	length = name ? strlen(name) : 0;
	if (!length) {
		return ".";
	}

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
	char *ret;
	pthread_attr_t attr;
	unsigned int stack_boundary = 0;
	unsigned int stack_size = 0;
	register int i;

	if (!pthread_getattr_np(pthread_self(), &attr)) {
		if (!pthread_attr_getstack(&attr, (void *)&stack_boundary, &stack_size)) {
			stack_boundary += stack_size;
		}
		pthread_attr_destroy(&attr);
	}

	ret = NULL;
	for (i = 0, stack = (int *)&stack; (unsigned int)stack < stack_boundary ; stack++, i++) {
		if (!dladdr((void *)*stack, &dinfo)) {
			continue;
		}

		
		ret = livebox_service_pkgname_by_libexec(dinfo.dli_fname);
		if (!ret) {
			DbgPrint("[%d] fname[%s] symbol[%s]\n", i, dinfo.dli_fname, dinfo.dli_sname);
			continue;
		}

		free(ret);
		ret = strdup(dinfo.dli_fname);

		if (symbol) {
			if (dinfo.dli_sname) {
				*symbol = strdup(dinfo.dli_sname);
			} else {
				*symbol = NULL;
			}
		}
		break;
	}

	return ret;
}

HAPI const char *util_uri_to_path(const char *uri)
{
	int len;

	len = strlen(SCHEMA_FILE);
	if (strncasecmp(uri, SCHEMA_FILE, len)) {
		return NULL;
	}

	return uri + len;
}

HAPI double util_time_delay_for_compensation(double period)
{
	unsigned long long curtime;
	unsigned long long _period;
	unsigned long long remain;
	struct timeval tv;
	double ret;

	if (period == 0.0f) {
		DbgPrint("Period is ZERO\n");
		return 0.0f;
	}

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("gettimeofday: %s\n", strerror(errno));
		return period;
	}

	curtime = (unsigned long long)tv.tv_sec * 1000000llu + (unsigned long long)tv.tv_usec;

	_period = (unsigned long long)(period * (double)1000000);
	if (_period == 0llu) {
		ErrPrint("%lf <> %llu\n", period, _period);
		return period;
	}

	remain = curtime % _period;

	ret = (double)remain / (double)1000000;
	return period - ret;
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;
	double delay;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer) {
		return NULL;
	}

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);

	return timer;
}

HAPI void util_timer_interval_set(void *timer, double interval)
{
	double delay;
	ecore_timer_interval_set(timer, interval);
	ecore_timer_reset(timer);

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);
}

/* End of a file */
