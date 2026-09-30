# Solana OS browser emulator (Emscripten). Build with ./build.sh.
#
# Compiles the badge's portable sources (SOLANA_OS_SRCS from
# main/sources.cmake, unchanged) and LVGL from managed_components, over an
# emulated hardware layer:
#   shim/      ESP-IDF, FreeRTOS and driver headers
#   platform/  their implementations, plus one <name>_emu.c counterpart for
#              every badge-only source (SOLANA_OS_DEVICE_SRCS and the BSP)
cmake_minimum_required(VERSION 3.16)
project(solana_os_emulator C)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (see build.sh)")
endif()

set(EMU ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(FW ${EMU}/.. ABSOLUTE)
set(SOLANA_OS_MAIN ${FW}/main CACHE PATH "Firmware main/ directory (sources.cmake, apps, core)")
set(MAIN ${SOLANA_OS_MAIN})
set(BSP ${FW}/components/bsp)
set(LVGL ${FW}/managed_components/lvgl__lvgl)
set(GEN ${CMAKE_BINARY_DIR}/gen)
set(EMU_OUT_DIR "${FW}/../web/static/emulator" CACHE PATH "Where solana-os.js/.wasm go")

if(NOT EXISTS ${LVGL}/lvgl.h)
    message(FATAL_ERROR "LVGL not found in ${LVGL}. Run 'idf.py reconfigure' in firmware/ to fetch managed components.")
endif()

# ---- Firmware sources: the same list the badge builds ----
include(${MAIN}/sources.cmake)
set(FW_SRCS ${SOLANA_OS_SRCS})
list(TRANSFORM FW_SRCS PREPEND ${MAIN}/)

# ---- Counterparts for badge-only sources: platform/<name>_emu.c ----
file(GLOB BSP_SRCS RELATIVE ${BSP} CONFIGURE_DEPENDS ${BSP}/*.c)
set(COUNTERPART_SRCS)
foreach(src ${SOLANA_OS_DEVICE_SRCS} ${BSP_SRCS})
    get_filename_component(stem ${src} NAME_WE)
    set(emu_src ${EMU}/platform/${stem}_emu.c)
    if(NOT EXISTS ${emu_src})
        message(FATAL_ERROR
            "Badge-only source '${src}' has no emulator counterpart.\n"
            "Add firmware/emulator/platform/${stem}_emu.c implementing the same header "
            "over emulated hardware (see firmware/emulator/README.md).")
    endif()
    list(APPEND COUNTERPART_SRCS ${emu_src})
endforeach()

# app_main_emu.c must bring up the same services as app_main.c: fail when
# app_main.c calls an os_/bsp_/console_ function its counterpart does not.
file(READ ${MAIN}/app_main.c APP_MAIN_SRC)
file(READ ${EMU}/platform/app_main_emu.c APP_MAIN_EMU_SRC)
string(REGEX MATCHALL "(os|bsp|console)_[a-z0-9_]+\\(" boot_calls "${APP_MAIN_SRC}")
list(REMOVE_DUPLICATES boot_calls)
foreach(call ${boot_calls})
    string(FIND "${APP_MAIN_EMU_SRC}" "${call}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "app_main.c calls ${call}...) but platform/app_main_emu.c does not. "
                            "Mirror the boot change in the emulator counterpart.")
    endif()
endforeach()

# cJSON: the badge uses ESP-IDF's copy (the "json" component); so do we.
if(NOT CJSON_DIR)
    set(idf_candidates $ENV{IDF_PATH} $ENV{HOME}/.espressif/v5.5.4/esp-idf)
    file(GLOB more_idf $ENV{HOME}/.espressif/v5.5*/esp-idf $ENV{HOME}/esp/esp-idf)
    foreach(idf ${idf_candidates} ${more_idf})
        if(EXISTS ${idf}/components/json/cJSON/cJSON.c)
            set(CJSON_DIR ${idf}/components/json/cJSON CACHE PATH "cJSON sources (ESP-IDF components/json/cJSON)")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS ${CJSON_DIR}/cJSON.c)
    message(FATAL_ERROR "cJSON not found: set IDF_PATH or -DCJSON_DIR=<esp-idf>/components/json/cJSON")
endif()

# Emulator runtime: scheduler, ESP-IDF services, simulated radios.
set(PLATFORM_SRCS
    ${EMU}/platform/emu_main.c
    ${EMU}/platform/freertos_emu.c
    ${EMU}/platform/esp_emu.c
    ${EMU}/platform/esp_wifi_emu.c
    ${CJSON_DIR}/cJSON.c
)
set_source_files_properties(${CJSON_DIR}/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")

# ---- sdkconfig.h from the badge's sdkconfig (LVGL + firmware CONFIG_*) ----
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_CMD ${Python3_EXECUTABLE} ${EMU}/tools/gen_sdkconfig.py ${FW} ${GEN}/sdkconfig.h)
execute_process(COMMAND ${GEN_CMD} RESULT_VARIABLE gen_rc)
if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "gen_sdkconfig.py failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FW}/sdkconfig ${FW}/sdkconfig.defaults)

set(COMMON_DEFS SOLANA_OS_EMULATOR=1 "LV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"")
set(COMMON_INCS ${EMU}/shim ${GEN} ${MAIN} ${BSP}/include ${LVGL} ${EMU}/platform ${CJSON_DIR})
# os_cloud.c identity hooks: the server sees model "emulator", hw "emu-<mac>".
set(FW_DEFS "OS_CLOUD_MODEL=\"emulator\"" "OS_CLOUD_HW_PREFIX=\"emu-\"")

# ---- LVGL ----
file(GLOB_RECURSE LVGL_SRCS CONFIGURE_DEPENDS ${LVGL}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_compile_definitions(lvgl PUBLIC ${COMMON_DEFS})
target_include_directories(lvgl PUBLIC ${COMMON_INCS})
target_compile_options(lvgl PRIVATE -Os -w)

# ---- Solana OS ----
add_executable(solana-os ${FW_SRCS} ${COUNTERPART_SRCS} ${PLATFORM_SRCS})
target_link_libraries(solana-os PRIVATE lvgl)
target_compile_definitions(solana-os PRIVATE ${FW_DEFS})
# -Wall like the badge; clang also flags write-only statics that GCC accepts.
target_compile_options(solana-os PRIVATE -Os -Wall -Wno-unused-but-set-global)
set_target_properties(solana-os PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${EMU_OUT_DIR})
set(EMU_LINK_OPT -Os CACHE STRING "Link-time (wasm-opt) optimization level")
target_link_options(solana-os PRIVATE
    ${EMU_LINK_OPT}
    -sASYNCIFY=1
    -sASYNCIFY_STACK_SIZE=16384
    -sMODULARIZE=1
    -sEXPORT_NAME=SolanaOS
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=16MB
    -sSTACK_SIZE=256KB
    -sEXIT_RUNTIME=0
    -sEXPORTED_FUNCTIONS=_main
    -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8,HEAPU32
    -sINCOMING_MODULE_JS_API=print,printErr,locateFile,onAbort
)

list(LENGTH FW_SRCS n_fw)
list(LENGTH COUNTERPART_SRCS n_cp)
message(STATUS "Solana OS emulator: ${n_fw} shared firmware sources, ${n_cp} device counterparts")
