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

extern int update_monitor_init(void);
extern int update_monitor_fini(void);

extern int update_monitor_add_update_cb(const char *filename, int (*cb)(const char *filename, void *data, int over), void *data);
extern int update_monitor_add_delete_cb(const char *filename, int (*cb)(const char *filename, void *data, int over), void *data);
extern void *update_monitor_del_update_cb(const char *filename, int (*cb)(const char *filename, void *data, int over));
extern void *update_monitor_del_delete_cb(const char *filename, int (*cb)(const char *filename, void *data, int over));
extern int update_monitor_trigger_update_cb(const char *filename, int over);
extern int update_monitor_trigger_delete_cb(const char *filename, int over);

// End of a file
