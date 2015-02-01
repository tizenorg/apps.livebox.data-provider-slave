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
#include <Ecore.h>

#include <dynamicbox_provider.h>
#include <dynamicbox_conf.h>

#include "critical_log.h"
#include "main.h"
#include "debug.h"
#include "fault.h"
#include "client.h"
#include "util.h"
#include "conf.h"

static struct info {
#if defined(_USE_ECORE_TIME_GET)
    double alarm_tv;
#else
    struct timeval alarm_tv;
#endif
    int marked;
    int disable_checker;
    struct sigaction SEGV_act;
    struct sigaction ILL_act;
    struct sigaction ALRM_act;
    struct sigaction USR1_act;
    struct sigaction ABRT_act;
    char **argv;
} s_info = {
    .marked = 0,
    .disable_checker = 0,
    .argv = NULL,
};

static void signal_handler(int signum, siginfo_t *info, void *unused)
{
    char *so_fname;
    char *symbol = NULL;

    so_fname = util_get_current_module(&symbol);

    if (info->si_signo == SIGALRM) {
	if (!s_info.marked) {
	    DbgPrint("Ignore false alarm signal [false]\n");
	    return;
	}

#if defined(_USE_ECORE_TIME_GET)
	double tv;
	tv = ecore_time_get();
	if (tv - s_info.alarm_tv < DEFAULT_LIFE_TIMER) {
	    DbgPrint("Ignore false alarm signal [%lf]\n", tv - s_info.alarm_tv);
	    return;
	}

	CRITICAL_LOG("ALARM: %lf (%d, %d)\n", tv - s_info.alarm_tv, DEFAULT_LIFE_TIMER, DEFAULT_LOAD_TIMER);
#else
	struct timeval tv;
	struct timeval res_tv;

	if (gettimeofday(&tv, NULL) < 0) {
	    ErrPrint("gettimeofday: %s\n", strerror(errno));
	    tv.tv_sec = 0;
	    tv.tv_usec = 0;
	}

	timersub(&tv, &s_info.alarm_tv, &res_tv);

	/*!
	 * \note
	 * 	GAP: 1 sec
	 */
	if (res_tv.tv_sec < DEFAULT_LIFE_TIMER) {
	    DbgPrint("Ignore false alarm signal [%d]\n", res_tv.tv_sec);
	    return;
	}

	CRITICAL_LOG("ALARM: %d.%d (%d, %d)\n",
		res_tv.tv_sec, res_tv.tv_usec, DEFAULT_LIFE_TIMER, DEFAULT_LOAD_TIMER);
#endif
    } else if (so_fname) {
	int fd;
	char log_fname[256];

	snprintf(log_fname, sizeof(log_fname), "%s/slave.%d", DYNAMICBOX_CONF_LOG_PATH, getpid());
	fd = open(log_fname, O_WRONLY|O_CREAT|O_SYNC, 0644);
	if (fd >= 0) {
	    if (write(fd, so_fname, strlen(so_fname)) != strlen(so_fname)) {
		ErrPrint("Failed to recording the fault SO filename (%s)\n", so_fname);
	    }
	    if (close(fd) < 0) {
		ErrPrint("close: %s\n", strerror(errno));
	    }
	}
    }

    CRITICAL_LOG("SIGNAL> Received from PID[%d]\n", info->si_pid);
    CRITICAL_LOG("SIGNAL> Signal: %d (%d)\n", signum, info->si_signo);
    CRITICAL_LOG("SIGNAL> Error code: %d\n", info->si_code);
    CRITICAL_LOG("SIGNAL> Address: %p\n", info->si_addr);
    CRITICAL_LOG("Package: [%s] Symbol[%s]\n", so_fname, symbol);

    if (so_fname) {
	int len = strlen(s_info.argv[0]);
	memset(s_info.argv[0], 0, len);
	strncpy(s_info.argv[0], util_basename(so_fname), len - 1);
	free(so_fname);
    } else {
	CRITICAL_LOG("Unable to find a so_fname (%s)\n", symbol);
    }

    free(symbol);

    switch (signum) {
	case SIGSEGV:
	    s_info.SEGV_act.sa_sigaction(signum, info, unused);
	    break;
	case SIGALRM:
	    s_info.ALRM_act.sa_sigaction(signum, info, unused);
	    break;
	case SIGABRT:
	    s_info.ABRT_act.sa_sigaction(signum, info, unused);
	    break;
	case SIGILL:
	    s_info.ILL_act.sa_sigaction(signum, info, unused);
	    break;
	case SIGUSR1:
	    s_info.USR1_act.sa_sigaction(signum, info, unused);
	    break;
	default:
	    ErrPrint("Unhandled signal\n");
	    break;
    }
}

HAPI int fault_init(char **argv)
{
    struct sigaction act;
    char *ecore_abort;

    s_info.argv = argv;

    act.sa_sigaction = signal_handler;
    act.sa_flags = SA_SIGINFO;

    if (sigemptyset(&act.sa_mask) != 0) {
	ErrPrint("Failed to init signal: %s\n", strerror(errno));
    }

    if (sigaddset(&act.sa_mask, SIGUSR1) != 0) {
	ErrPrint("Failed to add set: %s\n", strerror(errno));
    }

    if (sigaddset(&act.sa_mask, SIGALRM) != 0) {
	ErrPrint("Failed to add set: %s\n", strerror(errno));
    }

    ecore_abort = getenv("ECORE_ERROR_ABORT");
    if (!ecore_abort || ecore_abort[0] != '1') {
	if (sigaddset(&act.sa_mask, SIGSEGV) != 0) {
	    ErrPrint("Failed to add set: %s\n", strerror(errno));
	}
	if (sigaddset(&act.sa_mask, SIGABRT) != 0) {
	    ErrPrint("Failed to add set: %s\n", strerror(errno));
	}
	if (sigaddset(&act.sa_mask, SIGILL) != 0) {
	    ErrPrint("Failed to add set: %s\n", strerror(errno));
	}

	if (sigaction(SIGSEGV, &act, &s_info.SEGV_act) < 0) {
	    ErrPrint("Failed to install the SEGV handler\n");
	}

	if (sigaction(SIGABRT, &act, &s_info.ABRT_act) < 0) {
	    ErrPrint("Faield to install the ABRT handler\n");
	}

	if (sigaction(SIGILL, &act, &s_info.ILL_act) < 0) {
	    ErrPrint("Faield to install the ILL handler\n");
	}
    }

    if (sigaction(SIGUSR1, &act, &s_info.USR1_act) < 0) {
	ErrPrint("Failed to install the USR1 handler\n");
    }

    if (sigaction(SIGALRM, &act, &s_info.ALRM_act) < 0) {
	ErrPrint("Failed to install the ALRM handler\n");
    }

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
    if (!s_info.disable_checker) {
	dynamicbox_provider_send_call(pkgname, filename, funcname);
    }
    /*!
     * \NOTE
     *   To use this "alarm", the dynamicbox have to do not use the 'sleep' series functions.
     *   because those functions will generate alarm signal.
     *   then the module will be deactivated
     *
     *   Enable alarm for detecting infinite loop
     */
    if (!noalarm) {
#if defined(_USE_ECORE_TIME_GET)
	s_info.alarm_tv = ecore_time_get();
#else
	if (gettimeofday(&s_info.alarm_tv, NULL) < 0) {
	    ErrPrint("gettimeofday: %s\n", strerror(errno));
	    s_info.alarm_tv.tv_sec = 0;
	    s_info.alarm_tv.tv_usec = 0;
	}
#endif
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

    if (!s_info.disable_checker) {
	dynamicbox_provider_send_ret(pkgname, filename, funcname);
    }
    return 0;
}

HAPI void fault_disable_call_option(void)
{
    s_info.disable_checker = 1;
}

/* End of a file */
