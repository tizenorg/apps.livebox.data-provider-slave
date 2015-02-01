#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <Eina.h>

#include <com-core.h>
#include <packet.h>
#include <secure_socket.h>
#include <com-core_packet.h>
#include <dynamicbox_errno.h>

#include <dlog.h>

#include "debug.h"
#include "connection.h"

static struct info {
    Eina_List *connection_list;
    Eina_List *connected_list;
    Eina_List *disconnected_list;

    enum {
	IDLE = 0x00,
	DISCONNECTION = 0x01,
	CONNECTION = 0x02,
    } process;
} s_info = {
    .connection_list = NULL,
    .connected_list = NULL,
    .disconnected_list = NULL,
    .process = IDLE,
};

int errno;

struct event_item {
    int (*event_cb)(int handle, void *data);
    void *data;
    int deleted;
};

struct connection {
    char *addr;
    int fd;
    int refcnt;
};

/**
 * When we get this connected callback,
 * The connection handle is not prepared yet.
 * So it is not possible to find a connection handle using socket fd.
 * In this case, just propagate this event to upper layer.
 * Make them handles this.
 */
static int connected_cb(int handle, void *data)
{
    Eina_List *l;
    Eina_List *n;
    struct event_item *item;

    EINA_LIST_FOREACH_SAFE(s_info.connected_list, l, n, item) {
	s_info.process = DISCONNECTION;
	if (item->deleted || item->event_cb(handle, item->data) < 0 || item->deleted) {
	    s_info.connected_list = eina_list_remove(s_info.connected_list, item);
	    free(item);
	}
	s_info.process = IDLE;
    }

    return 0;
}

static int disconnected_cb(int handle, void *data)
{
    Eina_List *l;
    Eina_List *n;
    struct event_item *item;

    EINA_LIST_FOREACH_SAFE(s_info.disconnected_list, l, n, item) {
	s_info.process = DISCONNECTION;
	if (item->deleted || item->event_cb(handle, item->data) < 0 || item->deleted) {
	    s_info.disconnected_list = eina_list_remove(s_info.disconnected_list, item);
	    free(item);
	}
	s_info.process = IDLE;
    }

    return 0;
}

int connection_init(void)
{
    if (com_core_add_event_callback(CONNECTOR_DISCONNECTED, disconnected_cb, NULL) < 0) {
	ErrPrint("Unable to register the disconnected callback\n");
    }

    if (com_core_add_event_callback(CONNECTOR_CONNECTED, connected_cb, NULL) < 0) {
	ErrPrint("Unable to register the disconnected callback\n");
    }

    return DBOX_STATUS_ERROR_NONE;
}

int connection_fini(void)
{
    (void)com_core_del_event_callback(CONNECTOR_DISCONNECTED, disconnected_cb, NULL);
    (void)com_core_del_event_callback(CONNECTOR_CONNECTED, connected_cb, NULL);

    return DBOX_STATUS_ERROR_NONE;
}

struct connection *connection_create(const char *addr, void *table)
{
    struct connection *handle;

    handle = calloc(1, sizeof(*handle));
    if (!handle) {
	ErrPrint("calloc: %s\n", strerror(errno));
	return NULL;
    }

    handle->addr = strdup(addr);
    if (!handle->addr) {
	ErrPrint("strdup: %s (%s)\n", strerror(errno), addr);
	free(handle);
	return NULL;
    }

    handle->fd = com_core_packet_client_init(handle->addr, 0, table);
    if (handle->fd < 0) {
	ErrPrint("Unable to make a connection %s\n", handle->addr);
	free(handle->addr);
	free(handle);
	return NULL;
    }

    handle->refcnt = 1;

    s_info.connection_list = eina_list_append(s_info.connection_list, handle);
    return handle;
}

struct connection *connection_ref(struct connection *handle)
{
    if (!handle) {
	return NULL;
    }

    handle->refcnt++;
    return handle;
}

struct connection *connection_unref(struct connection *handle)
{
    if (!handle) {
	return NULL;
    }

    handle->refcnt--;
    if (handle->refcnt > 0) {
	return handle;
    }

    s_info.connection_list = eina_list_remove(s_info.connection_list, handle);

    if (handle->fd >= 0) {
	com_core_packet_client_fini(handle->fd);
    }

    free(handle->addr);
    free(handle);
    return NULL;
}

struct connection *connection_find_by_addr(const char *addr)
{
    Eina_List *l;
    struct connection *handle;

    if (!addr) {
	return NULL;
    }

    EINA_LIST_FOREACH(s_info.connection_list, l, handle) {
	if (handle->addr && !strcmp(handle->addr, addr)) {
	    return handle;
	}
    }

    return NULL;
}

struct connection *connection_find_by_fd(int fd)
{
    Eina_List *l;
    struct connection *handle;

    if (fd < 0) {
	return NULL;
    }

    EINA_LIST_FOREACH(s_info.connection_list, l, handle) {
	if (handle->fd == fd) {
	    return handle;
	}
    }

    return NULL;
}

int connection_add_event_handler(enum connection_event_type type, int (*event_cb)(int handle, void *data), void *data)
{
    struct event_item *item;

    item = malloc(sizeof(*item));
    if (!item) {
	ErrPrint("malloc: %s\n", strerror(errno));
	return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
    }

    item->event_cb = event_cb;
    item->data = data;

    switch (type) {
	case CONNECTION_EVENT_TYPE_CONNECTED:
	    s_info.connected_list = eina_list_append(s_info.connected_list, item);
	    break;
	case CONNECTION_EVENT_TYPE_DISCONNECTED:
	    s_info.disconnected_list = eina_list_append(s_info.disconnected_list, item);
	    break;
	default:
	    free(item);
	    return DBOX_STATUS_ERROR_INVALID_PARAMETER;
    }

    return DBOX_STATUS_ERROR_NONE;
}

void *connection_del_event_handler(enum connection_event_type type, int (*event_cb)(int handle, void *data))
{
    Eina_List *l;
    Eina_List *n;
    struct event_item *item;
    void *ret = NULL;

    switch (type) {
	case CONNECTION_EVENT_TYPE_CONNECTED:
	    EINA_LIST_FOREACH_SAFE(s_info.connected_list, l, n, item) {
		if (item->event_cb == event_cb) {
		    if (s_info.process == CONNECTION) {
			item->deleted = 1;
			ret = item->data;
		    } else {
			s_info.connected_list = eina_list_remove(s_info.connected_list, item);
			ret = item->data;
			free(item);
		    }
		    break;
		}
	    }
	    break;
	case CONNECTION_EVENT_TYPE_DISCONNECTED:
	    EINA_LIST_FOREACH_SAFE(s_info.disconnected_list, l, n, item) {
		if (item->event_cb == event_cb) {
		    if (s_info.process == DISCONNECTION) {
			item->deleted = 1;
			ret = item->data;
		    } else {
			s_info.disconnected_list = eina_list_remove(s_info.disconnected_list, item);
			ret = item->data;
			free(item);
		    }
		    break;
		}
	    }
	    break;
	default:
	    break;
    }

    return ret;
}

int connection_handle(struct connection *connection)
{
    return connection->fd;
}

const char *connection_addr(struct connection *connection)
{
    return connection->addr;
}

/* End of a file */
