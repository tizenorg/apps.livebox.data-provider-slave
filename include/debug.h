/*
 * Copyright 2012  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.tizenopensource.org/license
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DbgPrint(format, arg...) LOGD("[[32m%s/%s[0m:%d] " format, util_basename(__FILE__), __func__, __LINE__, ##arg)

#define ErrPrint(format, arg...) LOGE("[[32m%s/%s[0m:%d] [33m" format "[0m", util_basename(__FILE__), __func__, __LINE__, ##arg)

#define WarnPrint(format, arg...) LOGW("[[32m%s/%s[0m:%d] [34m" format "[0m", util_basename(__FILE__), __func__, __LINE__, ##arg)

#define DbgFree(a) do { \
	free(a); \
} while (0)

/* End of a file */
