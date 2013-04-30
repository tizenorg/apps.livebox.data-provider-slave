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

extern int lb_init(void);
extern int lb_fini(void);

extern int lb_create(const char *pkgname, const char *id, const char *content_info, int timeout, int has_livebox_script, double period, const char *cluster, const char *category, int *w, int *h, double *priority, int skip_need_to_create, const char *abi, char **out_content, char **out_title);
extern int lb_destroy(const char *pkgname, const char *id);

extern int lb_resize(const char *pkgname, const char *id, int w, int h);
extern int lb_clicked(const char *pkgname, const char *id, const char *event, double timestamp, double x, double y);

extern int lb_script_event(const char *pkgname, const char *id, const char *emission, const char *source, struct event_info *event_info);
extern int lb_change_group(const char *pkgname, const char *id, const char *cluster, const char *category);

extern int lb_update(const char *pkgname, const char *id);
extern int lb_update_all(const char *pkgname, const char *cluster, const char *category);
extern void lb_pause_all(void);
extern void lb_resume_all(void);
extern int lb_set_period(const char *pkgname, const char *id, double period);
extern char *lb_pinup(const char *pkgname, const char *id, int pinup);
extern int lb_system_event(const char *pkgname, const char *id, int event);
extern int lb_system_event_all(int event);

extern int lb_open_pd(const char *pkgname);
extern int lb_close_pd(const char *pkgname);

extern int lb_pause(const char *pkgname, const char *id);
extern int lb_resume(const char *pkgname, const char *id);

extern int lb_is_pinned_up(const char *pkgname, const char *id);

extern void lb_turn_secured_on(void);

/*!
 * Exported API for each liveboxes
 */
extern const char *livebox_find_pkgname(const char *filename);
extern int livebox_request_update_by_id(const char *filename);

/* End of a file */
