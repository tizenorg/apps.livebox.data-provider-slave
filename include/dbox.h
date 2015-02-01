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

extern void dbox_init(void);
extern void dbox_fini(void);

struct dbox_create_arg {
	double period;
	int timeout;
	int has_dynamicbox_script;
	int skip_need_to_create;
	const char *content;
	const char *cluster;
	const char *category;
	const char *abi;
	const char *direct_addr;
};

extern int dbox_create(const char *pkgname, const char *id, struct dbox_create_arg *arg, int *w, int *h, double *priority, char **content, char **title);
extern int dbox_destroy(const char *pkgname, const char *id, int type);

extern int dbox_resize(const char *pkgname, const char *id, int w, int h);
extern int dbox_clicked(const char *pkgname, const char *id, const char *event, double timestamp, double x, double y);
extern int dbox_set_content_info(const char *pkgname, const char *id, const char *content_info);
extern int dbox_set_content_info_all(const char *pkgname, const char *content);

extern int dbox_script_event(const char *pkgname, const char *id, const char *emission, const char *source, dynamicbox_event_info_t event_info);
extern int dbox_change_group(const char *pkgname, const char *id, const char *cluster, const char *category);

extern int dbox_update(const char *pkgname, const char *id, int force);
extern int dbox_update_all(const char *pkgname, const char *cluster, const char *category, int force);
extern void dbox_pause_all(void);
extern void dbox_resume_all(void);
extern int dbox_set_period(const char *pkgname, const char *id, double period);
extern char *dbox_pinup(const char *pkgname, const char *id, int pinup);
extern int dbox_system_event(const char *pkgname, const char *id, int event);
extern int dbox_system_event_all(int event);

extern int dbox_open_gbar(const char *pkgname, const char *id);
extern int dbox_close_gbar(const char *pkgname, const char *id);

extern int dbox_pause(const char *pkgname, const char *id);
extern int dbox_resume(const char *pkgname, const char *id);

extern int dbox_is_pinned_up(const char *pkgname, const char *id);

extern void dbox_turn_secured_on(void);
extern int dbox_is_secured(void);

extern int dbox_is_all_paused(void);
extern int dbox_delete_all_deleteme(void);
extern int dbox_delete_all(void);

/**
 * @brief
 * Exported API for each dynamicboxes
 */
extern const char *dynamicbox_find_pkgname(const char *filename);
extern int dynamicbox_request_update_by_id(const char *filename);
extern int dynamicbox_trigger_update_monitor(const char *id, int is_gbar);
extern int dynamicbox_update_extra_info(const char *id, const char *content, const char *title, const char *icon, const char *name);
extern int dynamicbox_send_updated(const char *pkgname, const char *id, int idx, int x, int y, int w, int h, int gbar, const char *descfile);
extern int dynamicbox_send_buffer_updated(const char *pkgname, const char *id, dynamicbox_buffer_h handle, int idx, int x, int y, int w, int h, int gbar, const char *descfile);

/* End of a file */
