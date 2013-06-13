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

#if !defined(SECURE_LOGD)
#define SECURE_LOGD LOGD
#endif

#if !defined(SECURE_LOGE)
#define SECURE_LOGE LOGE
#endif

#if !defined(SECURE_LOGW)
#define SECURE_LOGW LOGW
#endif

#define DbgPrint(format, arg...) SECURE_LOGD("[[32m%s/%s[0m:%d] " format, util_basename(__FILE__), __func__, __LINE__, ##arg)

#define ErrPrint(format, arg...) SECURE_LOGE("[[32m%s/%s[0m:%d] [33m" format "[0m", util_basename(__FILE__), __func__, __LINE__, ##arg)

#define WarnPrint(format, arg...) SECURE_LOGW("[[32m%s/%s[0m:%d] [34m" format "[0m", util_basename(__FILE__), __func__, __LINE__, ##arg)

#define DbgFree(a) do { \
	free(a); \
} while (0)

/* End of a file */
