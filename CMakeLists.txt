CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(data-provider-slave C)

INCLUDE(FindPkgConfig)
pkg_check_modules(pkg REQUIRED
	capi-appfw-application
	capi-appfw-app-manager
	ail
	dlog
	aul
	vconf
	sqlite3
	db-util
	glib-2.0
	gio-2.0
	bundle
	ecore
	dynamicbox_provider
	dynamicbox_service
	edje
	evas
	dynamicbox
	elementary
	capi-system-system-settings
	com-core
	shortcut
	efl-assist
	json-glib-1.0
)

IF (X11_SUPPORT)
pkg_check_modules(pkg_extra REQUIRED
	ecore-x
)
ENDIF (X11_SUPPORT)

IF (WAYLAND_SUPPORT)
pkg_check_modules(pkg_extra REQUIRED
	ecore-wayland
)
ENDIF (WAYLAND_SUPPORT)

SET(PACKAGE "${PROJECT_NAME}")
SET(LOCALEDIR "/usr/apps/org.tizen.${PROJECT_NAME}/res/locale")
SET(ICON_PROVIDER "icon-provider-slave")
SET(EXE_LOADER "${PROJECT_NAME}")
SET(THEME_DIR "/opt/usr/share/data-provider-slave/theme")

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)

#SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")

ADD_DEFINITIONS("-DPREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
ADD_DEFINITIONS("-DPATH_MAX=256")
ADD_DEFINITIONS("-DPACKAGE=\"${PACKAGE}\"")
ADD_DEFINITIONS("-DLOCALEDIR=\"${LOCALEDIR}\"")
ADD_DEFINITIONS("-DEXEC_NAME=\"${PROJECT_NAME}.loader\"")

ADD_DEFINITIONS("-DMASTER_PKGNAME=\"data-provider-master\"")
ADD_DEFINITIONS("-DSLAVE_PKGNAME=\"org.tizen.data-provider-slave\"")
ADD_DEFINITIONS("-DSOCKET_FILE=\"/opt/usr/share/live_magazine/.live.socket\"")
ADD_DEFINITIONS("-DCONF_FILE=\"/usr/share/data-provider-master/conf.ini\"")

ADD_DEFINITIONS("-DNDEBUG")
#ADD_DEFINITIONS("-D_ENABLE_MCHECK")
ADD_DEFINITIONS("-DLOG_TAG=\"DATA_PROVIDER_SLAVE\"")
ADD_DEFINITIONS("-D_USE_ECORE_TIME_GET")
ADD_DEFINITIONS("-DTHEME_DIR=\"${THEME_DIR}\"")
#ADD_DEFINITIONS("-D_ESTIMATE_PERF")

ADD_DEFINITIONS(${pkg_CFLAGS})
ADD_DEFINITIONS(${pkg_LDFLAGS})
ADD_DEFINITIONS(${pkg_extra_CFLAGS})
ADD_DEFINITIONS(${pkg_extra_LDFLAGS})

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Werror -g -fno-builtin-malloc -fpie")

SET(BUILD_SOURCE
    src/main.c
    src/so_handler.c
    src/fault.c
    src/update_monitor.c
    src/util.c
    src/dbox.c
    src/client.c
    src/critical_log.c
    src/connection.c
    src/theme_loader.c
)

IF (WAYLAND_SUPPORT)
	ADD_DEFINITIONS("-DHAVE_WAYLAND")
	SET(BUILD_SOURCE 
		${BUILD_SOURCE}
		src/util_wayland.c
	)
ENDIF (WAYLAND_SUPPORT)

IF (X11_SUPPORT)
	ADD_DEFINITIONS("-DHAVE_X11")
	SET(BUILD_SOURCE 
		${BUILD_SOURCE}
		src/util_x11.c
	)
ENDIF (X11_SUPPORT)

ADD_EXECUTABLE("${PROJECT_NAME}"
	${BUILD_SOURCE}
)

TARGET_LINK_LIBRARIES(${PROJECT_NAME} "-ldl -pie" ${pkg_extra_LDFLAGS} ${pkg_LDFLAGS})

ADD_EXECUTABLE(${ICON_PROVIDER}
	icon_src/main.c
	icon_src/script_handler.c
	src/util.c
)
TARGET_LINK_LIBRARIES(${ICON_PROVIDER} "-ldl -pie" ${pkg_extra_LDFLAGS} ${pkg_LDFLAGS})

#INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.desktop DESTINATION /usr/share/applications)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.xml DESTINATION /usr/share/packages)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME "org.tizen.${PROJECT_NAME}")
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.efl DESTINATION /etc/smack/accesses.d)
INSTALL(TARGETS ${ICON_PROVIDER} DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
INSTALL(TARGETS ${EXE_LOADER} DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# INCLUDE FOR BUILD & INSTALL .PO FILES
ADD_SUBDIRECTORY(res)
ADD_SUBDIRECTORY(data)
