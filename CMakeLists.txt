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
	heap-monitor
	livebox-service
	edje
	evas
	livebox
	elementary
)

SET(PACKAGE "${PROJECT_NAME}")
SET(LOCALEDIR "/usr/apps/org.tizen.${PROJECT_NAME}/res/locale")

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)

#SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")

ADD_DEFINITIONS("-DPREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
ADD_DEFINITIONS("-DPATH_MAX=256")
ADD_DEFINITIONS("-DPACKAGE=\"${PACKAGE}\"")
ADD_DEFINITIONS("-DLOCALEDIR=\"${LOCALEDIR}\"")
ADD_DEFINITIONS("-DEXEC_NAME=\"${PROJECT_NAME}\"")

ADD_DEFINITIONS("-DMASTER_PKGNAME=\"org.tizen.data-provider-master\"")
ADD_DEFINITIONS("-DSLAVE_PKGNAME=\"org.tizen.data-provider-slave\"")
ADD_DEFINITIONS("-DSOCKET_FILE=\"/opt/usr/share/live_magazine/.live.socket\"")

ADD_DEFINITIONS("-DNDEBUG")
#ADD_DEFINITIONS("-D_ENABLE_MCHECK")
ADD_DEFINITIONS("-DLOG_TAG=\"DATA_PROVIDER_SLAVE\"")

ADD_DEFINITIONS(${pkg_CFLAGS})

# -fpie
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Werror -g -fno-builtin-malloc")
#SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")

ADD_EXECUTABLE(${PROJECT_NAME}
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

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${pkg_LDFLAGS} "-ldl")
#INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.desktop DESTINATION /usr/share/applications)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.xml DESTINATION /usr/share/packages)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME "org.tizen.${PROJECT_NAME}")
INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/org.tizen.data-provider-slave.rule DESTINATION /etc/smack/accesses2.d)

# INCLUDE FOR BUILD & INSTALL .PO FILES
ADD_SUBDIRECTORY(res)
ADD_SUBDIRECTORY(data)
