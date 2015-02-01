#include "util.h"

#include <dynamicbox_errno.h>

void *util_screen_get(void)
{
    return NULL;
}

int util_screen_size_get(int *width, int *height)
{
    *width = 0;
    *height = 0;
    return DBOX_STATUS_ERROR_NOT_IMPLEMENTED;
}

int util_screen_init(void)
{
    return DBOX_STATUS_ERROR_NONE;
}

int util_screen_fini(void)
{
    return DBOX_STATUS_ERROR_NONE;
}

/* End of a file */
