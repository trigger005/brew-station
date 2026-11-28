# Host-side unit tests (run on the Mac/PC, no ESP-IDF needed).
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(gbs_aku_bridge_host_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_aku_frame test_aku_frame.c ${ROOT}/components/aku/aku_frame.c)
target_include_directories(test_aku_frame PRIVATE ${ROOT}/components/aku/include)

enable_testing()
add_test(NAME aku_frame COMMAND test_aku_frame)

# machine/ uses cJSON for request bodies; take ESP-IDF's copy so host and
# target parse the same way.
if(DEFINED ENV{IDF_PATH})
    set(IDF_DIR $ENV{IDF_PATH})
else()
    set(IDF_DIR $ENV{HOME}/esp/esp-idf-v5.5)
endif()
set(CJSON_DIR ${IDF_DIR}/components/json/cJSON)
add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})
target_compile_options(cjson PRIVATE -w)

add_executable(test_machine test_machine.c
    ${ROOT}/components/machine/machine.c
    ${ROOT}/components/machine/machine_wire.c)
target_include_directories(test_machine PRIVATE ${ROOT}/components/machine/include)
target_link_libraries(test_machine PRIVATE cjson m)
add_test(NAME machine COMMAND test_machine)

add_executable(test_shot test_shot.c ${ROOT}/components/shot/shot.c ${ROOT}/components/shot/shot_decide.c
    ${ROOT}/components/shot/scale_clock.c)
target_include_directories(test_shot PRIVATE ${ROOT}/components/shot/include)
target_link_libraries(test_shot PRIVATE m)
add_test(NAME shot COMMAND test_shot)
