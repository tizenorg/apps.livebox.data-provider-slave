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
	ecore-x
	ecore
	provider
	livebox-service
	edje
	evas
	livebox
	elementary
	capi-system-system-settings
	com-core
	shortcut
	efl-assist
	json-glib-1.0
)

SET(PACKAGE "${PROJECT_NAME}")
SET(LOCALEDIR "/usr/apps/org.tizen.${PROJECT_NAME}/res/locale")
SET(ICON_PROVIDER "icon-provider-slave")
SET(EXE_LOADER "${PROJECT_NAME}")

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
#ADD_DEFINITIONS("-D_ESTIMATE_PERF")

ADD_DEFINITIONS(${pkg_CFLAGS})

FOREACH(flag ${pkg_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
	SET(EXTRA_CXXFLAGS "${EXTRA_CXXFLAGS} ${flag}")
ENDFOREACH(flag)

IF (MOBILE)
SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} -DMOBILE")
ENDIF (MOBILE)

IF (WEARABLE)
SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} -DWEARABLE")
ENDIF (WEARABLE)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -Wall")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_CXXFLAGS} -Wall")

# -fpie
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Werror -g -fno-builtin-malloc")
#SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")

SET(EXTRA_LDFLAGS "-ldl")
FOREACH(flag ${pkg_LDFLAGS})
	IF (flag STREQUAL -L)
		MESSAGE("Ignore -L")
	ELSE()
		SET(EXTRA_LDFLAGS "${EXTRA_LDFLAGS} ${flag}")
	ENDIF()
ENDFOREACH(flag)
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${EXTRA_LDFLAGS}")

ADD_EXECUTABLE("${PROJECT_NAME}"
	src/main.c
	src/so_handler.c
	src/fault.c
	src/update_monitor.c
	src/conf.c
	src/util.c
	src/lb.c
	src/client.c
	src/critical_log.c
)
TARGET_LINK_LIBRARIES("${PROJECT_NAME}" ${EXTRA_LDFLAGS})

ADD_EXECUTABLE(${ICON_PROVIDER}
	icon_src/main.c
	icon_src/script_handler.c
	src/util.c
)
TARGET_LINK_LIBRARIES(${ICON_PROVIDER} ${EXTRA_LDFLAGS})

#INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.desktop DESTINATION /usr/share/applications)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.xml DESTINATION /usr/share/packages)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME "org.tizen.${PROJECT_NAME}")

IF (WEARABLE)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.rule DESTINATION /etc/smack/accesses2.d)
ELSE (WEARABLE)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.rule DESTINATION /opt/etc/smack/accesses.d)
ENDIF (WEARABLE)


INSTALL(TARGETS ${ICON_PROVIDER} DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
INSTALL(TARGETS ${EXE_LOADER} DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# INCLUDE FOR BUILD & INSTALL .PO FILES
ADD_SUBDIRECTORY(res)
ADD_SUBDIRECTORY(data)
