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

#define DEFAULT_LIFE_TIMER 20
#define DEFAULT_LOAD_TIMER 20
#define MINIMUM_UPDATE_INTERVAL 0.1f

/**
 * @note
 * NO_ALARM is used for disabling the alarm code
 * This will turn off the alarm for checking the return of dynamicbox functions
 */
#define NO_ALARM 1

/**
 * @note
 * This is default action.
 * This will enable the alarm for checking the return time of dynamicbox functions
 * If the function doesn't return before alarm rining, it will be deal as a faulted one
 */
#define USE_ALARM 0

#if !defined(LOCALEDIR)
#define LOCALEDIR "/usr/share/locale"
#endif

#define HAPI __attribute__((visibility("hidden")))

/* End of a file */
