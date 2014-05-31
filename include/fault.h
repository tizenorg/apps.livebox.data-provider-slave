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

extern int fault_init(char **argv);
extern int fault_fini(void);
extern int fault_mark_call(const char *pkgname, const char *filename, const char *funcname, int noalarm, int life_time);
extern int fault_unmark_call(const char *pkgname, const char *filename, const char *funcname, int noalarm);
extern void fault_disable_call_option(void);

/* End of a file */
