ADD_CUSTOM_TARGET(icon.edj ALL
		COMMAND edje_cc -sd ${CMAKE_CURRENT_SOURCE_DIR} -id ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/icon.edc icon.edj
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icon.edc
)
ADD_DEPENDENCIES(${PROJECT_NAME} icon_font.edj)
ADD_CUSTOM_TARGET(icon_font.edj ALL
		COMMAND edje_cc -sd ${CMAKE_CURRENT_SOURCE_DIR} -id ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/icon_font.edc icon_font.edj
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icon_font.edc
)
ADD_DEPENDENCIES(${PROJECT_NAME} icon_font.edj)

ADD_CUSTOM_TARGET(icon_effect.edj ALL
		COMMAND edje_cc -sd ${CMAKE_CURRENT_SOURCE_DIR} -id ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/icon_effect.edc icon_effect.edj
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icon_effect.edc
)
ADD_DEPENDENCIES(${PROJECT_NAME} icon_effect.edj)
ADD_CUSTOM_TARGET(icon_effect_font.edj ALL
		COMMAND edje_cc -sd ${CMAKE_CURRENT_SOURCE_DIR} -id ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/icon_effect_font.edc icon_effect_font.edj
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icon_effect_font.edc
)
ADD_DEPENDENCIES(${PROJECT_NAME} icon_effect_font.edj)

INSTALL(FILES icon.edj DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/res/edje)
INSTALL(FILES icon_effect.edj DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/res/edje)
INSTALL(FILES icon_font.edj DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/res/edje)
INSTALL(FILES icon_effect_font.edj DESTINATION /usr/apps/org.tizen.${PROJECT_NAME}/res/edje)
