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
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <execinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include <dlog.h>
#include <Eina.h>

#include <provider.h>

#include "critical_log.h"
#include "main.h"
#include "debug.h"
#include "fault.h"
#include "conf.h"
#include "client.h"
#include "util.h"

static struct info {
	struct timeval alarm_tv;
	int marked;
	int disable_checker;
} s_info = {
	.marked = 0,
	.disable_checker = 0,
};

static void signal_handler(int signum, siginfo_t *info, void *unused)
{
	char *so_fname;
	char *symbol = NULL;

	so_fname = util_get_current_module(&symbol);

	if (info->si_signo == SIGALRM) {
		struct timeval tv;
		struct timeval res_tv;

		if (!s_info.marked) {
			DbgPrint("Ignore false alarm signal [false]\n");
			return;
		}

		gettimeofday(&tv, NULL);

		timersub(&tv, &s_info.alarm_tv, &res_tv);

		/*!
		 * \note
		 * 	GAP: 1 sec
		 */
		if (res_tv.tv_sec < DEFAULT_LIFE_TIMER) {
			DbgPrint("Ignore false alarm signal [%d]\n", res_tv.tv_sec);
			return;
		}

		DbgPrint("ALARM: %d.%d (%d, %d)\n",
				res_tv.tv_sec, res_tv.tv_usec, DEFAULT_LIFE_TIMER, DEFAULT_LOAD_TIMER);
	} else if (so_fname) {
		int fd;
		char log_fname[256];

		snprintf(log_fname, sizeof(log_fname), "%s/slave.%d", SLAVE_LOG_PATH, getpid());
		fd = open(log_fname, O_WRONLY|O_CREAT|O_SYNC, 0644);
		if (fd >= 0) {
			if (write(fd, so_fname, strlen(so_fname)) != strlen(so_fname))
				ErrPrint("Failed to recording the fault SO filename (%s)\n", so_fname);
			close(fd);
		}
	}

	ErrPrint("=====\n");
	ErrPrint("SIGNAL> Received from PID[%d]\n", info->si_pid);
	ErrPrint("SIGNAL> Signal: %d (%d)\n", signum, info->si_signo);
	ErrPrint("SIGNAL> Error code: %d\n", info->si_code);
	ErrPrint("SIGNAL> Address: %p\n", info->si_addr);
	ErrPrint("Package: [%s] Symbol[%s]\n", so_fname, symbol);
	free(so_fname);
	free(symbol);

	_exit(0);
	return;
}

HAPI int fault_init(void)
{
	struct sigaction act;

	if (access("/tmp/live.err", F_OK) == 0)
		ErrPrint("Error log still exists (/tmp/live.err)\n");

	act.sa_sigaction = signal_handler;
	act.sa_flags = SA_SIGINFO;

	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGSEGV);
	sigaddset(&act.sa_mask, SIGABRT);
	sigaddset(&act.sa_mask, SIGILL);
	sigaddset(&act.sa_mask, SIGUSR1);
	sigaddset(&act.sa_mask, SIGALRM);

	if (sigaction(SIGSEGV, &act, NULL) < 0)
		ErrPrint("Failed to install the SEGV handler\n");

	if (sigaction(SIGABRT, &act, NULL) < 0)
		ErrPrint("Faield to install the ABRT handler\n");

	if (sigaction(SIGILL, &act, NULL) < 0)
		ErrPrint("Faield to install the ILL handler\n");

	if (sigaction(SIGUSR1, &act, NULL) < 0)
		ErrPrint("Failed to install the USR1 handler\n");

	if (sigaction(SIGALRM, &act, NULL) < 0)
		ErrPrint("Failed to install the ALRM handler\n");

	return 0;
}

HAPI int fault_fini(void)
{
	/*!
	 * \todo
	 * remove all signal handlers
	 */
	return 0;
}

HAPI int fault_mark_call(const char *pkgname, const char *filename, const char *funcname, int noalarm, int life_time)
{
	if (!s_info.disable_checker)
		provider_send_call(pkgname, filename, funcname);
	/*!
	 * \NOTE
	 *   To use this "alarm", the livebox have to do not use the 'sleep' series functions.
	 *   because those functions will generate alarm signal.
	 *   then the module will be deactivated
	 *
	 *   Enable alarm for detecting infinite loop
	 */
	if (!noalarm) {
		gettimeofday(&s_info.alarm_tv, NULL);
		s_info.marked = 1;
		alarm(life_time);
	}

	return 0;
}

HAPI int fault_unmark_call(const char *pkgname, const char *filename, const char *funcname, int noalarm)
{
	if (!noalarm) {
		/*!
		 * \NOTE
		 * Disable alarm
		 */
		alarm(0);
		s_info.marked = 0;
	}

	if (!s_info.disable_checker)
		provider_send_ret(pkgname, filename, funcname);
	return 0;
}

HAPI void fault_disable_call_option(void)
{
	s_info.disable_checker = 1;
}

/* End of a file */
