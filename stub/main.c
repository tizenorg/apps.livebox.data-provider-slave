#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include <dlog.h>

#include "debug.h"
#if defined(LOG_TAG)
#undef LOG_TAG
#define LOG_TAG "DATA_PROVIDER_SLAVE_LOADER"
#endif

int errno;

const char *dynamicbox_find_pkgname(const char *filename)
{
	return NULL;
}

int dynamicbox_request_update_by_id(const char *filename)
{
	return 0;
}

int dynamicbox_trigger_update_monitor(const char *filename, int is_pd)
{
	return 0;
}

int main(int argc, char *argv[])
{
	int i;
	char **_argv;
	const char *option;

	if (argc < 4) {
		return -EINVAL;
	}

	_argv = malloc(sizeof(char *) * (argc+2));
	if (!_argv) {
		ErrPrint("%s\n", strerror(errno));
		return -ENOMEM;
	}

	for (i = 1; i < argc; i++) {
		_argv[i] = strdup(argv[i]);
	}
	_argv[i] = NULL;

	_argv[0] = strdup("/usr/apps/org.tizen.data-provider-slave/bin/data-provider-slave.loader");
	DbgPrint("Replace argv[0] with %s\n", _argv[0]);
	for (i = 0; i < argc; i++) {
		DbgPrint("argv[%d]: %s\n", i, _argv[i]);
	}

	option = getenv("PROVIDER_HEAP_MONITOR_START");
	if (option && !strcasecmp(option, "true")) {
		DbgPrint("Heap monitor is enabled\n");
		setenv("LD_PRELOAD", "/usr/lib/libheap-monitor.so", 1);
	}

	execvp(_argv[0], _argv);
	ErrPrint("%s\n", strerror(errno));
	return 0;
}

/* End of a file */
