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
#include <string.h>
#include <errno.h>
#include<stdlib.h>
#include <ctype.h>

#include <Elementary.h>
#include <Evas.h>
#include <Eina.h>
#include <Edje.h>
#include <Ecore.h>

#include <dlog.h>
#include <livebox-errno.h>

#include "debug.h"
#include "util.h"

#if defined(LOG_TAG)
#undef LOG_TAG
#define LOG_TAG "ICON_PROVIDER"
#endif

#define HAPI __attribute__((visibility("hidden")))
#define TYPE_COLOR "color"
#define TYPE_TEXT "text"
#define TYPE_IMAGE "image"
#define TYPE_EDJE "script"
#define TYPE_SIGNAL "signal"
#define TYPE_INFO "info"
#define TYPE_DRAG "drag"
#define TYPE_ACCESS "access"

#define INFO_SIZE "size"
#define INFO_CATEGORY "category"
#define ADDEND 256

struct block {
	char *type;
	int type_len;

	char *part;
	int part_len;

	char *data;
	int data_len;

	char *file;
	int file_len;

	char *option;
	int option_len;

	char *id;
	int id_len;

	char *target_id;
	int target_len;
};

struct image_option {
	int orient;
	int aspect;
	enum {
		FILL_DISABLE,
		FILL_IN_SIZE,
		FILL_OVER_SIZE,
	} fill;

	int width;
	int height;
};

struct child {
	Evas_Object *obj;
	char *part;
};

struct obj_info {
	char *id;
	Eina_List *children;
};

static struct info {
	Eina_List *obj_list;
} s_info = {
	.obj_list = NULL,
};

static inline Evas_Object *find_edje(const char *id)
{
	Eina_List *l;
	Evas_Object *child;
	struct obj_info *obj_info;

	EINA_LIST_FOREACH(s_info.obj_list, l, child) {
		obj_info = evas_object_data_get(child, "obj_info");
		if (!obj_info || strcmp(obj_info->id, id))
			continue;

		return child;
	}

	return NULL;
}

static inline void delete_block(struct block *block)
{
	DbgFree(block->file);
	DbgFree(block->type);
	DbgFree(block->part);
	DbgFree(block->data);
	DbgFree(block->option);
	DbgFree(block->id);
	DbgFree(block->target_id);
	DbgFree(block);
}

static int update_script_color(Evas_Object *edje, struct block *block)
{
	int r[3], g[3], b[3], a[3];
	int ret;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (block->id) {
		edje = find_edje(block->id);
		if (!edje) {
			ErrPrint("Edje is not exists: %s\n", block->id);
			return LB_STATUS_ERROR_NOT_EXIST;
		}
		DbgPrint("EDJE[%s] is selected (%p)\n", block->id, edje);
	}

	ret = sscanf(block->data, "%d %d %d %d %d %d %d %d %d %d %d %d",
					r, g, b, a,			/* OBJECT */
					r + 1, g + 1, b + 1, a + 1,	/* OUTLINE */
					r + 2, g + 2, b + 2, a + 2);	/* SHADOW */
	if (ret != 12) {
		DbgPrint("id[%s] part[%s] rgba[%s]\n", block->id, block->part, block->data);
		return LB_STATUS_ERROR_INVALID;
	}

	ret = edje_object_color_class_set(elm_layout_edje_get(edje), block->part,
				r[0], g[0], b[0], a[0], /* OBJECT */
				r[1], g[1], b[1], a[1], /* OUTLINE */
				r[2], g[2], b[2], a[2]); /* SHADOW */

	DbgPrint("color class is %s changed", ret == EINA_TRUE ? "successfully" : "not");
	return LB_STATUS_SUCCESS;
}

static int update_script_text(Evas_Object *edje, struct block *block)
{
	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (block->id) {
		edje = find_edje(block->id);
		if (!edje) {
			ErrPrint("Failed to find EDJE\n");
			return LB_STATUS_ERROR_NOT_EXIST;
		}
	}

	elm_object_part_text_set(edje, block->part, block->data ? block->data : "");

	return LB_STATUS_SUCCESS;
}

static void parse_aspect(struct image_option *img_opt, const char *value, int len)
{
	while (len > 0 && *value == ' ') {
		value++;
		len--;
	}

	if (len < 4)
		return;

	img_opt->aspect = !strncasecmp(value, "true", 4);
	DbgPrint("Parsed ASPECT: %d (%s)\n", img_opt->aspect, value);
}

static void parse_orient(struct image_option *img_opt, const char *value, int len)
{
	while (len > 0 && *value == ' ') {
		value++;
		len--;
	}

	if (len < 4)
		return;

	img_opt->orient = !strncasecmp(value, "true", 4);
	DbgPrint("Parsed ORIENT: %d (%s)\n", img_opt->orient, value);
}

static void parse_size(struct image_option *img_opt, const char *value, int len)
{
	int width;
	int height;
	char *buf;

	while (len > 0 && *value == ' ') {
		value++;
		len--;
	}

	buf = strndup(value, len);
	if (!buf) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}

	if (sscanf(buf, "%dx%d", &width, &height) == 2) {
		img_opt->width = width;
		img_opt->height = height;
		DbgPrint("Parsed size : %dx%d (%s)\n", width, height, buf);
	} else {
		DbgPrint("Invalid size tag[%s]\n", buf);
	}

	free(buf);
}

static void parse_fill(struct image_option *img_opt, const char *value, int len)
{
	while (len > 0 && *value == ' ') {
		value++;
		len--;
	}

	if (!strncasecmp(value, "in-size", len))
		img_opt->fill = FILL_IN_SIZE;
	else if (!strncasecmp(value, "over-size", len))
		img_opt->fill = FILL_OVER_SIZE;
	else
		img_opt->fill = FILL_DISABLE;

	DbgPrint("Parsed FILL: %d (%s)\n", img_opt->fill, value);
}

static inline void parse_image_option(const char *option, struct image_option *img_opt)
{
	const char *ptr;
	const char *cmd;
	const char *value;
	struct {
		const char *cmd;
		void (*handler)(struct image_option *img_opt, const char *value, int len);
	} cmd_list[] = {
		{
			.cmd = "aspect", /* Keep the aspect ratio */
			.handler = parse_aspect,
		},
		{
			.cmd = "orient", /* Keep the orientation value: for the rotated images */
			.handler = parse_orient,
		},
		{
			.cmd = "fill", /* Fill the image to its container */
			.handler = parse_fill, /* Value: in-size, over-size, disable(default) */
		},
		{
			.cmd = "size",
			.handler = parse_size,
		},
	};
	enum {
		STATE_START,
		STATE_TOKEN,
		STATE_DATA,
		STATE_IGNORE,
		STATE_ERROR,
		STATE_END,
	} state;
	int idx;
	int tag;

	if (!option || !*option)
		return;

	state = STATE_START;
	/*!
	 * \note
	 * GCC 4.7 warnings uninitialized idx and tag value.
	 * But it will be initialized by the state machine. :(
	 * Anyway, I just reset idx and tag for reducing the GCC4.7 complains.
	 */
	idx = 0;
	tag = 0;
	cmd = NULL;
	value = NULL;

	for (ptr = option; state != STATE_END; ptr++) {
		switch (state) {
		case STATE_START:
			if (*ptr == '\0') {
				state = STATE_END;
				continue;
			}

			if (isalpha(*ptr)) {
				state = STATE_TOKEN;
				ptr--;
			}
			tag = 0;
			idx = 0;

			cmd = cmd_list[tag].cmd;
			break;
		case STATE_IGNORE:
			if (*ptr == '=') {
				state = STATE_DATA;
				value = ptr;
			} else if (*ptr == '\0') {
				state = STATE_END;
			}
			break;
		case STATE_TOKEN:
			if (cmd[idx] == '\0' && (*ptr == ' ' || *ptr == '\t' || *ptr == '=')) {
				if (*ptr == '=') {
					value = ptr;
					state = STATE_DATA;
				} else {
					state = STATE_IGNORE;
				}
				idx = 0;
			} else if (*ptr == '\0') {
				state = STATE_END;
			} else if (cmd[idx] == *ptr) {
				idx++;
			} else {
				ptr -= (idx + 1);

				tag++;
				if (tag == sizeof(cmd_list) / sizeof(cmd_list[0])) {
					tag = 0;
					state = STATE_ERROR;
				} else {
					cmd = cmd_list[tag].cmd;
				}
				idx = 0;
			}
			break;
		case STATE_DATA:
			if (*ptr == ';' || *ptr == '\0') {
				cmd_list[tag].handler(img_opt, value + 1, idx);
				state = *ptr ? STATE_START : STATE_END;
			} else {
				idx++;
			}
			break;
		case STATE_ERROR:
			if (*ptr == ';')
				state = STATE_START;
			else if (*ptr == '\0')
				state = STATE_END;
			break;
		default:
			break;
		}
	}
}

static int update_script_image(Evas_Object *edje, struct block *block)
{
	Evas_Load_Error err;
	Evas_Object *img;
	Evas_Coord w, h;
	struct obj_info *obj_info;
	struct child *child;
	struct image_option img_opt = {
		.aspect = 0,
		.orient = 0,
		.fill = FILL_DISABLE,
		.width = -1,
		.height = -1,
	};

	if (block->id) {
		edje = find_edje(block->id);
		if (!edje) {
			ErrPrint("No such object: %s\n", block->id);
			return LB_STATUS_ERROR_NOT_EXIST;
		}
	}

	obj_info = evas_object_data_get(edje, "obj_info");
	if (!obj_info) {
		ErrPrint("Object info is not available\n");
		return LB_STATUS_ERROR_FAULT;
	}

	img = elm_object_part_content_unset(edje, block->part);
	if (img) {
		Eina_List *l;
		Eina_List *n;

		EINA_LIST_FOREACH_SAFE(obj_info->children, l, n, child) {
			if (child->obj != img)
				continue;

			obj_info->children = eina_list_remove(obj_info->children, child);
			free(child->part);
			free(child);
			break;
		}

		DbgPrint("delete object %s %p\n", block->part, img);
		evas_object_del(img);
	}

	if (!block->data || !strlen(block->data) || access(block->data, R_OK) != 0) {
		DbgPrint("SKIP - Path: [%s]\n", block->data);
		return LB_STATUS_SUCCESS;
	}

	child = malloc(sizeof(*child));
	if (!child) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	child->part = strdup(block->part);
	if (!child->part) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(child);
		return LB_STATUS_ERROR_MEMORY;
	}

	img = evas_object_image_add(evas_object_evas_get(edje));
	if (!img) {
		ErrPrint("Failed to add an image object\n");
		free(child->part);
		free(child);
		return LB_STATUS_ERROR_FAULT;
	}

	evas_object_image_preload(img, EINA_FALSE);
	parse_image_option(block->option, &img_opt);
	evas_object_image_load_orientation_set(img, img_opt.orient);

	evas_object_image_file_set(img, block->data, NULL);
	err = evas_object_image_load_error_get(img);
	if (err != EVAS_LOAD_ERROR_NONE) {
		ErrPrint("Load error: %s\n", evas_load_error_str(err));
		evas_object_del(img);
		free(child->part);
		free(child);
		return LB_STATUS_ERROR_IO;
	}

	evas_object_image_size_get(img, &w, &h);
	if (img_opt.aspect) {
		if (img_opt.fill == FILL_OVER_SIZE) {
			Evas_Coord part_w;
			Evas_Coord part_h;

			if (img_opt.width >= 0 && img_opt.height >= 0) {
				part_w = img_opt.width * elm_config_scale_get();
				part_h = img_opt.height * elm_config_scale_get();
			} else {
				part_w = 0;
				part_h = 0;
				edje_object_part_geometry_get(elm_layout_edje_get(edje), block->part, NULL, NULL, &part_w, &part_h);
			}
			DbgPrint("Original %dx%d (part: %dx%d)\n", w, h, part_w, part_h);

			if (part_w > w || part_h > h) {
				double fw;
				double fh;

				fw = (double)part_w / (double)w;
				fh = (double)part_h / (double)h;

				if (fw > fh) {
					w = part_w;
					h = (double)h * fw;
				} else {
					h = part_h;
					w = (double)w * fh;
				}
			}
			DbgPrint("Size: %dx%d\n", w, h);

			evas_object_image_load_size_set(img, w, h);
			evas_object_image_load_region_set(img, (w - part_w) / 2, (h - part_h) / 2, part_w, part_h);
			evas_object_image_fill_set(img, 0, 0, part_w, part_h);
			evas_object_image_reload(img);
		} else if (img_opt.fill == FILL_IN_SIZE) {
			Evas_Coord part_w;
			Evas_Coord part_h;

			if (img_opt.width >= 0 && img_opt.height >= 0) {
				part_w = img_opt.width * elm_config_scale_get();
				part_h = img_opt.height * elm_config_scale_get();
			} else {
				part_w = 0;
				part_h = 0;
				edje_object_part_geometry_get(elm_layout_edje_get(edje), block->part, NULL, NULL, &part_w, &part_h);
			}
			DbgPrint("Original %dx%d (part: %dx%d)\n", w, h, part_w, part_h);

			if (part_w > w || part_h > h) {
				double fw;
				double fh;

				fw = (double)part_w / (double)w;
				fh = (double)part_h / (double)h;

				if (fw > fh) {
					w = part_w;
					h = (double)h * fw;
				} else {
					h = part_h;
					w = (double)w * fh;
				}
			}
			DbgPrint("Size: %dx%d\n", w, h);
			evas_object_image_fill_set(img, 0, 0, part_w, part_h);
			evas_object_size_hint_fill_set(img, EVAS_HINT_FILL, EVAS_HINT_FILL);
			evas_object_size_hint_weight_set(img, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
		} else {
			evas_object_image_fill_set(img, 0, 0, w, h);
			evas_object_size_hint_fill_set(img, EVAS_HINT_FILL, EVAS_HINT_FILL);
			evas_object_size_hint_aspect_set(img, EVAS_ASPECT_CONTROL_BOTH, w, h);
		}
	} else {
		if (img_opt.width >= 0 && img_opt.height >= 0) {
			w = img_opt.width;
			h = img_opt.height;
			DbgPrint("Using given image size: %dx%d\n", w, h);
		}

		evas_object_image_fill_set(img, 0, 0, w, h);
		evas_object_image_filled_set(img, EINA_TRUE);
		evas_object_size_hint_fill_set(img, EVAS_HINT_FILL, EVAS_HINT_FILL);
		evas_object_size_hint_weight_set(img, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
	}

	/*!
	 * \note
	 * object will be shown by below statement automatically
	 */
	DbgPrint("%s part swallow image %p (%dx%d)\n", block->part, img, w, h);
	child->obj = img;
	elm_object_part_content_set(edje, block->part, img);
	obj_info->children = eina_list_append(obj_info->children, child);

	/*!
	 * \note
	 * This object is not registered as an access object.
	 * So the developer should add it to access list manually, using DESC_ACCESS block.
	 */
	return LB_STATUS_SUCCESS;
}

static void edje_del_cb(void *_info, Evas *e, Evas_Object *obj, void *event_info)
{
	struct obj_info *obj_info;
	struct child *child;
	Evas_Object *parent = _info;
	Eina_List *l;
	Eina_List *n;

	if (parent) {
		obj_info = evas_object_data_get(parent, "obj_info");
		if (!obj_info) {
			ErrPrint("Invalid parent\n");
			return;
		}

		EINA_LIST_FOREACH_SAFE(obj_info->children, l, n, child) {
			if (child->obj != obj)
				continue;

			obj_info->children = eina_list_remove(obj_info->children, child);
			free(child->part);
			free(child);
			break;
		}

		s_info.obj_list = eina_list_remove(s_info.obj_list, obj);
	} else {
		DbgPrint("Parent object is destroying\n");
	}

	obj_info = evas_object_data_del(obj, "obj_info");
	if (!obj_info) {
		ErrPrint("Object info is not valid\n");
		return;
	}

	DbgPrint("delete object %s %p\n", obj_info->id, obj);

	EINA_LIST_FREE(obj_info->children, child) {
		DbgPrint("delete object %s %p\n", child->part, child->obj);
		if (child->obj)
			evas_object_del(child->obj);

		free(child->part);
		free(child);
	}

	free(obj_info->id);
	free(obj_info);
}

static int update_script_script(Evas_Object *edje, struct block *block)
{
	Evas_Object *obj;
	struct obj_info *obj_info;
	struct child *child;
	struct obj_info *new_obj_info;

	DbgPrint("src_id[%s] target_id[%s] part[%s] path[%s] group[%s]\n", block->id, block->target_id, block->part, block->data, block->option);

	if (block->id) {
		edje = find_edje(block->id);
		if (!edje) {
			ErrPrint("Edje is not exists\n");
			return LB_STATUS_ERROR_NOT_EXIST;
		}
	}

	obj_info = evas_object_data_get(edje, "obj_info");
	if (!obj_info) {
		ErrPrint("Object info is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	obj = elm_object_part_content_unset(edje, block->part);
	if (obj) {
		Eina_List *l;
		Eina_List *n;

		EINA_LIST_FOREACH_SAFE(obj_info->children, l, n, child) {
			if (child->obj != obj)
				continue;

			obj_info->children = eina_list_remove(obj_info->children, child);

			free(child->part);
			free(child);
			break;
		}

		DbgPrint("delete object %s %p\n", block->part, obj);
		/*!
		 * \note
		 * This will call the edje_del_cb.
		 * It will delete all access objects
		 */
		evas_object_del(obj);
	}

	if (!block->data || !strlen(block->data) || access(block->data, R_OK) != 0) {
		DbgPrint("SKIP - Path: [%s]\n", block->data);
		return LB_STATUS_SUCCESS;
	}

	obj = elm_layout_add(edje);
	if (!obj) {
		ErrPrint("Failed to add a new edje object\n");
		return LB_STATUS_ERROR_FAULT;
	}

	if (!elm_layout_file_set(obj, block->data, block->option)) {
		int err;
		const char *errmsg;

		err = edje_object_load_error_get(elm_layout_edje_get(obj));
		errmsg = edje_load_error_str(err);
		ErrPrint("Could not load %s from %s: %s\n", block->option, block->data, errmsg);
		evas_object_del(obj);
		return LB_STATUS_ERROR_IO;
	}

	evas_object_show(obj);

	new_obj_info = calloc(1, sizeof(*obj_info));
	if (!new_obj_info) {
		ErrPrint("Failed to add a obj_info\n");
		evas_object_del(obj);
		return LB_STATUS_ERROR_MEMORY;
	}

	new_obj_info->id = strdup(block->target_id);
	if (!new_obj_info->id) {
		ErrPrint("Failed to add a obj_info\n");
		free(new_obj_info);
		evas_object_del(obj);
		return LB_STATUS_ERROR_MEMORY;
	}

	child = malloc(sizeof(*child));
	if (!child) {
		ErrPrint("Error: %s\n", strerror(errno));
		free(new_obj_info->id);
		free(new_obj_info);
		evas_object_del(obj);
		return LB_STATUS_ERROR_MEMORY;
	}

	child->part = strdup(block->part);
	if (!child->part) {
		ErrPrint("Error: %s\n", strerror(errno));
		free(child);
		free(new_obj_info->id);
		free(new_obj_info);
		evas_object_del(obj);
		return LB_STATUS_ERROR_MEMORY;
	}

	child->obj = obj;

	evas_object_data_set(obj, "obj_info", new_obj_info);
	evas_object_event_callback_add(obj, EVAS_CALLBACK_DEL, edje_del_cb, edje);
	s_info.obj_list = eina_list_append(s_info.obj_list, obj);

	DbgPrint("%s part swallow edje %p\n", block->part, obj);
	elm_object_part_content_set(edje, block->part, obj);
	obj_info->children = eina_list_append(obj_info->children, child);
	return LB_STATUS_SUCCESS;
}

static int update_script_signal(Evas_Object *edje, struct block *block)
{
	ErrPrint("Signal emit function is not supported\n");
	return LB_STATUS_ERROR_INVALID;
}

static int update_script_drag(Evas_Object *edje, struct block *block)
{
	ErrPrint("Signal emit function is not supported\n");
	return LB_STATUS_ERROR_INVALID;
}

static int update_info(Evas_Object *edje, struct block *block)
{
	ErrPrint("Signal emit function is not supported\n");
	return LB_STATUS_ERROR_INVALID;
}

static int update_access(Evas_Object *edje, struct block *block)
{
	ErrPrint("Accessibility is not able to be apply for making a shot\n");
	return LB_STATUS_ERROR_INVALID;
}

static inline void consuming_parsed_block(Evas_Object *edje, int lineno, struct block *block)
{
	/*!
	 * To speed up, use the static.
	 * But this will increase the memory slightly.
	 */
	static struct {
		const char *type;
		int (*handler)(Evas_Object *edje, struct block *block);
	} handlers[] = {
		{
			.type = TYPE_COLOR,
			.handler = update_script_color,
		},
		{
			.type = TYPE_TEXT,
			.handler = update_script_text,
		},
		{
			.type = TYPE_IMAGE,
			.handler = update_script_image,
		},
		{
			.type = TYPE_EDJE,
			.handler = update_script_script,
		},
		{
			.type = TYPE_SIGNAL,
			.handler = update_script_signal,
		},
		{
			.type = TYPE_DRAG,
			.handler = update_script_drag,
		},
		{
			.type = TYPE_INFO,
			.handler = update_info,
		},
		{
			.type = TYPE_ACCESS,
			.handler = update_access,
		},
		{
			.type = NULL,
			.handler = NULL,
		},
	};

	register int i;

	for (i = 0; handlers[i].type; i++) {
		if (strcasecmp(handlers[i].type, block->type))
			continue;

		handlers[i].handler(edje, block);
		break;
	}

	if (!handlers[i].type)
		ErrPrint("%d: Unknown block type: %s\n", lineno, block->type);

	delete_block(block);

	return;
}

HAPI int script_handler_parse_desc(Evas_Object *edje, const char *descfile)
{
	FILE *fp;
	int ch;
	int lineno;
	enum state {
		UNKNOWN = 0x10,
		BLOCK_OPEN = 0x11,
		FIELD = 0x12,
		VALUE = 0x13,
		BLOCK_CLOSE = 0x14,

		VALUE_TYPE = 0x00,
		VALUE_PART = 0x01,
		VALUE_DATA = 0x02,
		VALUE_FILE = 0x03,
		VALUE_OPTION = 0x04,
		VALUE_ID = 0x05,
		VALUE_TARGET = 0x06,
	};
	const char *field_name[] = {
		"type",
		"part",
		"data",
		"file",
		"option",
		"id",
		"target",
		NULL
	};
	enum state state;
	register int field_idx;
	register int idx = 0;
	struct block *block;
	struct obj_info *info;

	block = NULL;

	fp = fopen(descfile, "rt");
	if (!fp) {
		ErrPrint("Error: %s [%s]\n", descfile, strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	/*!
	 * \note
	 * After open a descfile, we can delete it.
	 */
	if (unlink(descfile) < 0)
		ErrPrint("Unable to delete file\n");

	DbgPrint("Parsing %s\n", descfile);
	DbgPrint("Building obj_info\n");
	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		fclose(fp);
		return LB_STATUS_ERROR_MEMORY;
	}
	info->id = NULL;
	info->children = NULL;

	evas_object_data_set(edje, "obj_info", info);
	evas_object_event_callback_add(edje, EVAS_CALLBACK_DEL, edje_del_cb, NULL);

	state = UNKNOWN;
	field_idx = 0;
	lineno = 1;

	block = NULL;
	while (!feof(fp)) {
		ch = getc(fp);
		if (ch == '\n')
			lineno++;

		switch (state) {
		case UNKNOWN:
			if (ch == '{') {
				state = BLOCK_OPEN;
				break;
			}

			if (!isspace(ch) && ch != EOF) {
				ErrPrint("%d: Syntax error: Desc is not started with '{' or space - (%c = 0x%x)\n", lineno, ch, ch);
				fclose(fp);
				return LB_STATUS_ERROR_INVALID;
			}
			break;

		case BLOCK_OPEN:
			if (isblank(ch))
				break;

			if (ch != '\n') {
				ErrPrint("%d: Syntax error: New line must has to be started right after '{'\n", lineno);
				goto errout;
			}

			block = calloc(1, sizeof(*block));
			if (!block) {
				ErrPrint("Heap: %s\n", strerror(errno));
				fclose(fp);
				return LB_STATUS_ERROR_MEMORY;
			}

			state = FIELD;
			idx = 0;
			field_idx = 0;
			break;

		case FIELD:
			if (isspace(ch))
				break;

			if (ch == '}') {
				state = BLOCK_CLOSE;
				break;
			}

			if (ch == '=') {
				if (field_name[field_idx][idx] != '\0') {
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				switch (field_idx) {
				case 0:
					state = VALUE_TYPE;
					if (block->type) {
						DbgFree(block->type);
						block->type = NULL;
						block->type_len = 0;
					}
					idx = 0;
					break;
				case 1:
					state = VALUE_PART;
					if (block->part) {
						DbgFree(block->part);
						block->part = NULL;
						block->part_len = 0;
					}
					idx = 0;
					break;
				case 2:
					state = VALUE_DATA;
					if (block->data) {
						DbgFree(block->data);
						block->data = NULL;
						block->data_len = 0;
					}
					idx = 0;
					break;
				case 3:
					state = VALUE_FILE;
					if (block->file) {
						DbgFree(block->file);
						block->file = NULL;
						block->file_len = 0;
					}
					idx = 0;
					break;
				case 4:
					state = VALUE_OPTION;
					if (block->option) {
						DbgFree(block->option);
						block->option = NULL;
						block->option_len = 0;
					}
					idx = 0;
					break;
				case 5:
					state = VALUE_ID;
					if (block->id) {
						DbgFree(block->id);
						block->id = NULL;
						block->id_len = 0;
					}
					idx = 0;
					break;
				case 6:
					state = VALUE_TARGET;
					if (block->target_id) {
						DbgFree(block->target_id);
						block->target_id = NULL;
						block->target_len = 0;
					}
					idx = 0;
					break;
				default:
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				break;
			}

			if (ch == '\n')
				goto errout;

			if (field_name[field_idx][idx] != ch) {
				ungetc(ch, fp);
				if (ch == '\n')
					lineno--;

				while (--idx >= 0)
					ungetc(field_name[field_idx][idx], fp);

				field_idx++;
				if (field_name[field_idx] == NULL) {
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				idx = 0;
				break;
			}

			idx++;
			break;

		case VALUE_TYPE:
			if (idx == block->type_len) {
				char *tmp;
				block->type_len += ADDEND;
				tmp = realloc(block->type, block->type_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->type = tmp;
			}

			if (ch == '\n') {
				block->type[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->type[idx] = ch;
			idx++;
			break;

		case VALUE_PART:
			if (idx == block->part_len) {
				char *tmp;
				block->part_len += ADDEND;
				tmp = realloc(block->part, block->part_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->part = tmp;
			}

			if (ch == '\n') {
				block->part[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->part[idx] = ch;
			idx++;
			break;

		case VALUE_DATA:
			if (idx == block->data_len) {
				char *tmp;
				block->data_len += ADDEND;
				tmp = realloc(block->data, block->data_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->data = tmp;
			}

			if (ch == '\n') {
				block->data[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->data[idx] = ch;
			idx++;
			break;

		case VALUE_FILE:
			if (idx == block->file_len) {
				char *tmp;
				block->file_len += ADDEND;
				tmp = realloc(block->file, block->file_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->file = tmp;
			}

			if (ch == '\n') {
				block->file[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->file[idx] = ch;
			idx++;
			break;

		case VALUE_OPTION:
			if (idx == block->option_len) {
				char *tmp;
				block->option_len += ADDEND;
				tmp = realloc(block->option, block->option_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->option = tmp;
			}

			if (ch == '\n') {
				block->option[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->option[idx] = ch;
			idx++;
			break;
		case VALUE_ID:
			if (idx == block->id_len) {
				char *tmp;
				block->id_len += ADDEND;
				tmp = realloc(block->id, block->id_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->id = tmp;
			}

			if (ch == '\n') {
				block->id[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->id[idx] = ch;
			idx++;
			break;
		case VALUE_TARGET:
			if (idx == block->target_len) {
				char *tmp;
				block->target_len += ADDEND;
				tmp = realloc(block->target_id, block->target_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->target_id = tmp;
			}

			if (ch == '\n') {
				block->target_id[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->target_id[idx] = ch;
			idx++;
			break;
		case BLOCK_CLOSE:
			if (!block->file) {
				block->file = strdup(descfile);
				if (!block->file) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
			}

			consuming_parsed_block(edje, lineno, block);
			state = UNKNOWN;
			break;

		default:
			break;
		} /* switch */
	} /* while */

	if (state != UNKNOWN) {
		ErrPrint("%d: Unknown state\n", lineno);
		goto errout;
	}

	fclose(fp);
	return LB_STATUS_SUCCESS;

errout:
	ErrPrint("Parse error at %d file %s\n", lineno, util_basename(descfile));
	if (block)
		delete_block(block);
	fclose(fp);
	return LB_STATUS_ERROR_INVALID;
}

/* End of a file */
