#include <Ecore_X.h>
#include <dlog.h>

#include <dynamicbox_errno.h>
#include "util.h"
#include "debug.h"

void *util_screen_get(void)
{
    return ecore_x_display_get();
}

int util_screen_size_get(int *width, int *height)
{
    ecore_x_window_size_get(0, width, height);
    return DBOX_STATUS_ERROR_NONE;
}

int util_screen_init(void)
{
    return ecore_x_init(NULL);
}

int util_screen_fini(void)
{
    return ecore_x_shutdown();
}

/* End of a file */

