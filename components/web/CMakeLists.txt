idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_http_server
    PRIV_REQUIRES aku autoshot shot net grinder_api machine esp_app_format esp_timer log freertos
    EMBED_TXTFILES "www/index.html"
)

# A new build number on every build (tools/build_info.cmake), shown in the
# page's Diagnostics. The target always runs, so web.c recompiles each time.
set(BUILD_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(web_build_info ALL
    COMMAND ${CMAKE_COMMAND} -DCOUNTER=${PROJECT_DIR}/build_number.txt -DOUT=${BUILD_INFO_H}
            -P ${PROJECT_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO_H}
    VERBATIM)
add_dependencies(${COMPONENT_LIB} web_build_info)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
