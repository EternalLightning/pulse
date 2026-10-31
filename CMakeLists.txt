# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

cmake_minimum_required(VERSION 3.25)
# Compiler detection and every later dependency scan must use the same language.
set(ENV{VSLANG} "1033")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PULSE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
project(Pulse VERSION ${PULSE_VERSION} LANGUAGES C CXX)
if(NOT DEFINED PROJECT_VERSION_TWEAK OR PROJECT_VERSION_TWEAK STREQUAL "")
    set(PROJECT_VERSION_TWEAK 0)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Self-contained releases, including bundled static dependencies.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Pulse requires MSVC (Windows SDK / D2D / NT API)")
endif()
foreach(language C CXX)
    string(STRIP "${CMAKE_${language}_CL_SHOWINCLUDES_PREFIX}" includes_prefix)
    if(CMAKE_GENERATOR MATCHES "NMake" AND includes_prefix MATCHES "[^ -~]")
        message(FATAL_ERROR
            "Localized MSVC include diagnostics are unsafe in this NMake build directory. "
            "Configure a fresh Ninja build directory with scripts/vcvars.bat; "
            "do not reuse objects with missing header dependencies.")
    endif()
    set(compiler_launcher cmd.exe /d /c call "${CMAKE_CURRENT_SOURCE_DIR}/scripts/msvc_compile.bat")
    if(CMAKE_${language}_COMPILER_LAUNCHER)
        list(APPEND compiler_launcher ${CMAKE_${language}_COMPILER_LAUNCHER})
    endif()
    set(CMAKE_${language}_COMPILER_LAUNCHER ${compiler_launcher})
endforeach()

add_compile_options(/utf-8 /permissive- /W4 /EHsc)
add_compile_options(/guard:cf)
add_link_options(/guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
# Expose optional SDK COM interfaces; runtime calls are capability-checked.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 _WIN32_IE=0x0A00 NTDDI_VERSION=NTDDI_WIN10_NI)

option(PULSE_WITH_LUMATEXT "Use LumaText for Pulse-owned single-line text" ON)
set(LUMATEXT_SOURCE_DIR "" CACHE PATH
    "Optional LumaText source tree used for development builds")
set(PULSE_LUMATEXT_TARGET "")
set(PULSE_LUMATEXT_RUNTIME_TARGET "")
if(PULSE_WITH_LUMATEXT)
    # The committed prebuilt package keeps everyday builds from rebuilding the
    # LumaText source tree; an explicit CMAKE_PREFIX_PATH (release SDK) still wins.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/bin/lumatext.dll")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext")
    endif()
    if(LUMATEXT_SOURCE_DIR)
        if(NOT EXISTS "${LUMATEXT_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "LUMATEXT_SOURCE_DIR does not contain CMakeLists.txt")
        endif()
        set(LUMATEXT_BUILD_STATIC OFF)
        set(LUMATEXT_BUILD_SAMPLES OFF)
        set(LUMATEXT_BUILD_TESTS OFF)
        set(LUMATEXT_BUILD_REFERENCE_TOOLS OFF)
        add_subdirectory("${LUMATEXT_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/_deps/lumatext" EXCLUDE_FROM_ALL)
        if(MSVC)
            foreach(runtime_target lumatext_shared lumatext_harfbuzz freetype)
                if(TARGET ${runtime_target})
                    set_property(TARGET ${runtime_target} PROPERTY MSVC_RUNTIME_LIBRARY
                        "${CMAKE_MSVC_RUNTIME_LIBRARY}")
                endif()
            endforeach()
        endif()
    else()
        find_package(LumaText 0.1 CONFIG QUIET)
    endif()

    if(TARGET lumatext_shared)
        set(PULSE_LUMATEXT_TARGET LumaText::D2D)
        set(PULSE_LUMATEXT_RUNTIME_TARGET lumatext_shared)
        message(STATUS "Pulse text renderer: LumaText shared library")
    elseif(TARGET LumaText::Shared)
        set(PULSE_LUMATEXT_TARGET LumaText::Shared)
        set(PULSE_LUMATEXT_RUNTIME_TARGET LumaText::Shared)
        message(STATUS "Pulse text renderer: installed LumaText shared library")
    elseif(TARGET LumaText::D2D)
        set(PULSE_LUMATEXT_TARGET LumaText::D2D)
        message(STATUS "Pulse text renderer: LumaText package")
    else()
        message(STATUS "Pulse text renderer: DirectWrite (LumaText was not found)")
    endif()
endif()

function(pulse_enable_lumatext target)
    target_sources(${target} PRIVATE src/ui/lumatext_renderer.cpp src/ui/edit_composition.cpp)
    if(PULSE_LUMATEXT_TARGET)
        target_compile_definitions(${target} PRIVATE PULSE_HAS_LUMATEXT=1)
        target_link_libraries(${target} PRIVATE ${PULSE_LUMATEXT_TARGET} delayimp)
        target_link_options(${target} PRIVATE /DELAYLOAD:lumatext.dll)
        if(PULSE_LUMATEXT_RUNTIME_TARGET)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${PULSE_LUMATEXT_RUNTIME_TARGET}>
                    $<TARGET_FILE_DIR:${target}>)
            if(LUMATEXT_SOURCE_DIR)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LUMATEXT_SOURCE_DIR}/LICENSE"
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText/LumaText.txt
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${LUMATEXT_SOURCE_DIR}/LICENSES"
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText)
            elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/share/LumaText/licenses")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/share/LumaText/licenses"
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText)
            endif()
        endif()
    endif()
endfunction()

set(PULSE_BUILD_ID_FILE "${CMAKE_BINARY_DIR}/pulse_build_id.txt")
if(DEFINED PULSE_BUILD_ID AND NOT PULSE_BUILD_ID STREQUAL "")
    file(WRITE "${PULSE_BUILD_ID_FILE}" "${PULSE_BUILD_ID}\n")
elseif(EXISTS "${PULSE_BUILD_ID_FILE}")
    file(STRINGS "${PULSE_BUILD_ID_FILE}" PULSE_BUILD_ID LIMIT_COUNT 1)
else()
    string(TIMESTAMP PULSE_BUILD_TIME "%Y%m%dT%H%M%SZ" UTC)
    string(RANDOM LENGTH 8 ALPHABET 0123456789ABCDEF PULSE_BUILD_RANDOM)
    set(PULSE_BUILD_ID "${PULSE_BUILD_TIME}-${PULSE_BUILD_RANDOM}")
    file(WRITE "${PULSE_BUILD_ID_FILE}" "${PULSE_BUILD_ID}\n")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(cmake/pulse_version.h.in generated/pulse_version.h @ONLY)
configure_file(cmake/pulse.manifest.in generated/pulse_app.manifest @ONLY)
set_source_files_properties(src/app/pulse.rc src/bench/child_edit_test.rc
    src/bench/dialog_edit_test.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/generated")
set_property(SOURCE src/app/pulse.rc src/bench/child_edit_test.rc
    src/bench/dialog_edit_test.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_BINARY_DIR}/generated/pulse_app.manifest")

function(pulse_add_version_resource target description internal_name original_filename)
    set(PULSE_FILE_DESCRIPTION "${description}")
    set(PULSE_INTERNAL_NAME "${internal_name}")
    set(PULSE_ORIGINAL_FILENAME "${original_filename}")
    configure_file(cmake/version.rc.in "generated/${target}_version.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated/${target}_version.rc")
endfunction()

add_library(pulse_crash_reporter STATIC
    src/common/crash_reporter.cpp
    src/common/diagnostics_exporter.cpp)
target_include_directories(pulse_crash_reporter PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_crash_reporter PUBLIC dbghelp shell32 pulse_localization)

add_library(pulse_text STATIC src/common/text_decode.cpp)

add_executable(pulse_text_compare_test src/bench/text_compare_test.cpp src/app/text_diff.cpp)
target_link_libraries(pulse_text_compare_test PRIVATE pulse_text)

add_executable(pulse_tab_indicator_animation_test src/bench/tab_indicator_animation_test.cpp)
target_compile_options(pulse_tab_indicator_animation_test PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_tab_indicator_animation_test PRIVATE /MANIFEST:NO)

add_library(pulse_user_storage STATIC src/common/user_storage.cpp)
target_link_libraries(pulse_user_storage PUBLIC shell32 ole32 advapi32 pulse_localization)
add_executable(pulse_user_storage_test src/bench/user_storage_test.cpp)
target_link_libraries(pulse_user_storage_test PRIVATE pulse_user_storage)
add_executable(pulse_user_index_storage_test
    src/bench/user_index_storage_test.cpp
    src/index/user_index_storage.cpp
    src/index/index_migration.cpp)
target_link_libraries(pulse_user_index_storage_test PRIVATE pulse_user_storage shell32 advapi32)

add_library(pulse_localization STATIC src/common/localization.cpp)
target_link_libraries(pulse_localization PUBLIC user32)

add_executable(pulse_localization_test
    src/bench/localization_test.cpp
    src/bench/localization_test.rc)
set_property(SOURCE src/bench/localization_test.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc")
target_link_libraries(pulse_localization_test PRIVATE pulse_localization)

add_executable(pulse_known_folder_labels_test
    src/bench/known_folder_labels_test.cpp
    src/app/entry_sort.cpp
    src/bench/localization_test.rc)
target_link_libraries(pulse_known_folder_labels_test PRIVATE pulse_localization shell32 ole32 uuid shlwapi)

add_executable(pulse_crash_test src/bench/crash_reporter_test.cpp)
target_link_libraries(pulse_crash_test PRIVATE pulse_crash_reporter)

add_executable(pulse_shell_host_log_test src/bench/shell_host_log_test.cpp)
add_executable(pulse_shell_registry_watch_test src/bench/shell_registry_watch_test.cpp)
target_link_libraries(pulse_shell_registry_watch_test PRIVATE user32)
add_executable(pulse_batch_rename_preview_test
    src/bench/batch_rename_preview_test.cpp src/model/batch_rename.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_batch_rename_preview_test PRIVATE ole32 shell32 uuid shlwapi netapi32)
add_executable(pulse_host_config_test src/bench/host_config_test.cpp
    src/index/content_configuration.cpp src/index/index_config.cpp)
target_link_libraries(pulse_host_config_test PRIVATE pulse_user_storage pulse_text pulse_sqlite advapi32 shell32 ole32)

# ---- Stage 0 benchmarks (console) ----
add_executable(pulse_bench_enum src/bench/measure_enum.cpp)
add_executable(pulse_bench_shell src/bench/measure_shell.cpp)
target_link_libraries(pulse_bench_shell PRIVATE ole32 shell32 uuid comctl32)
add_executable(pulse_bench_mft src/bench/measure_mft.cpp)

add_executable(pulse_index_test
    src/bench/index_test_main.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp
    src/index/search_result_merge.cpp
    src/index/change_tracking.cpp)
target_link_libraries(pulse_index_test PRIVATE shell32 mpr advapi32 pulse_user_storage)
target_sources(pulse_index_test PRIVATE src/bench/network_index_regression_test.cpp)
target_compile_definitions(pulse_index_test PRIVATE PULSE_NETWORK_INDEX_TESTING=1)

add_executable(pulse_search_merge_test
    src/bench/search_merge_test.cpp
    src/index/search_result_merge.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp)

add_executable(pulse_index_migration_test
    src/bench/index_migration_test.cpp
    src/index/index_migration.cpp)
target_link_libraries(pulse_index_migration_test PRIVATE shell32)

add_executable(pulse_index_engine_test
    src/bench/index_engine_test.cpp
    src/index/index_engine.cpp
    src/index/change_tracking.cpp
    src/index/index_visibility.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_mft.cpp
    src/index/index_paths.cpp
    src/index/index_delta.cpp
    src/index/index_shard.cpp
    src/fs/fs_enum.cpp)
target_link_libraries(pulse_index_engine_test PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr)

# Keep storage comparisons runnable without the unrelated legacy engine fixtures.
get_target_property(index_storage_sources pulse_index_engine_test SOURCES)
list(REMOVE_ITEM index_storage_sources src/bench/index_engine_test.cpp)
add_executable(pulse_index_storage_test src/bench/index_storage_pair_test.cpp ${index_storage_sources})
target_link_libraries(pulse_index_storage_test PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr)
add_executable(pulse_index_compaction_test src/bench/index_compaction_test.cpp ${index_storage_sources})
target_link_libraries(pulse_index_compaction_test PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr)
# The retired per-volume query implementation is compiled only for the A/B oracle.
target_compile_definitions(pulse_index_storage_test PRIVATE PULSE_INDEX_LEGACY_QUERY_SHARDS=1)
add_executable(pulse_index_search_cache_test src/bench/index_search_cache_test.cpp ${index_storage_sources})
target_link_libraries(pulse_index_search_cache_test PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr)

# End-to-end index latency probe. It talks to the installed/running service so
# timings include the production pipe, query engine, paging, and serialization.
add_executable(pulse_index_perf src/bench/index_perf_main.cpp)
add_executable(pulse_removable_index_policy_test src/bench/removable_index_policy_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/search_result_merge.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_removable_index_policy_test PRIVATE pulse_user_storage advapi32 shell32)

add_executable(pulse_index_host_stress src/bench/index_host_stress.cpp)

add_executable(pulse_content_search_test
    src/bench/content_search_test.cpp
    src/index/content_search.cpp
    src/index/content_search_client.cpp)
target_link_libraries(pulse_content_search_test PRIVATE pulse_text bcrypt ole32)
add_dependencies(pulse_content_search_test pulse_index)

add_executable(pulse_duplicate_scan_test
    src/bench/duplicate_scan_test.cpp
    src/app/duplicate_scan.cpp)
target_link_libraries(pulse_duplicate_scan_test PRIVATE ole32)

add_executable(pulse_saved_search_test
    src/bench/saved_search_test.cpp
    src/app/saved_search.cpp)
target_link_libraries(pulse_saved_search_test PRIVATE shell32 ole32 pulse_user_storage)

add_executable(pulse_search_query_test
    src/bench/search_query_test.cpp
    src/model/search_query.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp)

# ---- Stage 0 visual/perf sample: D2D virtual list (GUI) ----
add_executable(pulse_d2d_list WIN32 src/bench/d2d_list.cpp)
target_link_libraries(pulse_d2d_list PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore)

# Fluent component visual contract: dark/light/high-contrast regression PNGs.
add_executable(pulse_fluent_gallery WIN32
    src/bench/fluent_components_gallery.cpp
    src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp src/ui/dialog_lifecycle.cpp
    src/ui/window_helpers.cpp)
target_link_libraries(pulse_fluent_gallery PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32
    pulse_localization)
pulse_enable_lumatext(pulse_fluent_gallery)

# ---- Focused visual regression: offscreen commands and theme tokens ----
add_executable(pulse_command_icon_test src/bench/command_icon_test.cpp)
target_link_libraries(pulse_command_icon_test PRIVATE d2d1 dwrite dwmapi ole32 windowscodecs user32)

add_executable(pulse_date_group_layout_test src/bench/date_group_layout_test.cpp
    src/app/date_groups.cpp src/ui/view_layout.cpp)
add_library(pulse_shell_query_client STATIC src/ipc/shell_query_client.cpp)
add_library(pulse_shell_interaction_client STATIC
    src/ipc/shell_interaction_client.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_shell_interaction_client PUBLIC user32 advapi32)
target_link_libraries(pulse_shell_query_client PUBLIC advapi32 user32)
add_executable(pulse_home_layout_test src/bench/home_layout_test.cpp
    src/app/home_catalog.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_home_layout_test PRIVATE pulse_localization shell32 ole32 uuid pulse_shell_query_client)
add_executable(pulse_network_access_test src/bench/network_access_test.cpp
    src/app/network_connect.cpp src/app/network_locations.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_network_access_test PRIVATE pulse_localization ole32 oleaut32 uuid user32)

# Optional static include-boundary check; no third-party Python packages required.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(pulse_ui_dependency_check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ui_dependencies.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
endif()

# Focused architecture regressions; these fixtures never start installed agents.
add_executable(pulse_typography_cache_test src/bench/typography_cache_test.cpp)
target_link_libraries(pulse_typography_cache_test PRIVATE dwrite user32 gdi32)
add_executable(pulse_search_client_routing_test
    src/bench/search_client_routing_test.cpp
    src/index/index_client.cpp src/index/network_agent_client.cpp
    src/index/index_query.cpp src/index/search_result_merge.cpp src/index/search_kinds.cpp)
target_link_libraries(pulse_search_client_routing_test PRIVATE user32 shell32 advapi32)
add_executable(pulse_fs_enumeration_test
    src/bench/fs_enumeration_test.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_fs_enumeration_test PRIVATE shell32 shlwapi netapi32)
target_compile_definitions(pulse_fs_enumeration_test PRIVATE PULSE_FS_ENUM_TESTING=1)
add_executable(pulse_folder_sizes_scan_test
    src/bench/folder_sizes_scan_test.cpp src/app/folder_sizes.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_folder_sizes_scan_test PRIVATE pulse_fs_watch shell32 shlwapi netapi32 advapi32)
target_compile_definitions(pulse_folder_sizes_scan_test PRIVATE PULSE_FOLDER_SIZES_TESTING=1)
add_executable(pulse_details_meta_test src/bench/details_meta_test.cpp src/app/details_meta.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_details_meta_test PRIVATE pulse_localization advapi32 shell32)

# ---- Pulse.exe: main UI process ----
file(GLOB_RECURSE PULSE_APP_SOURCES CONFIGURE_DEPENDS
    src/app/*.cpp src/model/*.cpp src/ui/*.cpp src/fs/*.cpp src/ops/*.cpp src/ipc/*.cpp src/index/*.cpp)
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_host\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "network_agent_main\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_(agent|index|search|task_search|hit_aliases|instant_session)\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "filename_timing\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "image_open\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "device_fake\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "text_task_reader\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_timing\\.cpp$")

list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "lumatext_renderer\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "(document_(host_main|reader|filter)|office_ooxml|pdfium_text)\\.cpp$")

# The local-volume engine runs in Pulse.Index.exe. Production keeps only the
# lightweight query parser and network agent client inside Pulse.exe.
option(PULSE_WITH_SELFTEST "Embed the legacy --selftest suite in Pulse.exe" OFF)
if(NOT PULSE_WITH_SELFTEST)
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "selftest_1b2\\.cpp$")
    # Restrict this to app/: ui/ui_hit_test.cpp is production hit testing.
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "/app/[^/]+_test\\.cpp$")
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_(engine|engine_pinyin|visibility|mft|paths|delta|folder_sizes)\\.cpp$")
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "network_index\\.cpp$")
endif()
list(APPEND PULSE_APP_SOURCES src/index/network_agent_client.cpp)
if(PULSE_APP_SOURCES)
    add_executable(pulse WIN32 ${PULSE_APP_SOURCES})
    set_target_properties(pulse PROPERTIES OUTPUT_NAME "Pulse")
    add_custom_command(TARGET pulse POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/7zip" "$<TARGET_FILE_DIR:pulse>/7zip")
    if(PULSE_WITH_SELFTEST)
        target_compile_definitions(pulse PRIVATE PULSE_WITH_SELFTEST=1)
        target_compile_definitions(pulse_user_storage PRIVATE PULSE_WITH_SELFTEST=1)
    endif()
    target_sources(pulse PRIVATE src/app/pulse.rc)
    set_property(SOURCE src/app/pulse.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/global-search-logo.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/pulse.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/copy_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/star_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/history_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/desktop_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/arrow_square_down_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/recycle_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/drive_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/document_folder_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/globe_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/search_visual_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/apps_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/panel_right_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/files_toolbar/delete_color_dark.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/files_toolbar/group.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/panel_right_contract_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/pc_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/tag_24_color.svg")
    target_link_options(pulse PRIVATE /MANIFEST:NO)
    target_link_libraries(pulse PRIVATE
        d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid comctl32 advapi32 shlwapi version windowscodecs gdi32 netapi32 mpr bcrypt pulse_crash_reporter)
    target_link_libraries(pulse PRIVATE secur32 propsys oleaut32 cfgmgr32)
    # Keep optional system services out of the initial loader work. These are
    # actual Pulse imports; delay-loading unused import libraries emits warnings.
    target_link_libraries(pulse PRIVATE delayimp)
    target_link_options(pulse PRIVATE
        /DELAYLOAD:version.dll /DELAYLOAD:netapi32.dll /DELAYLOAD:mpr.dll
        /DELAYLOAD:secur32.dll /DELAYLOAD:cfgmgr32.dll /DELAYLOAD:rstrtmgr.dll)
    target_compile_options(pulse PRIVATE $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
    target_link_options(pulse PRIVATE $<$<CONFIG:Release>:/OPT:REF,ICF>)
    set_property(TARGET pulse PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    target_link_libraries(pulse PRIVATE pulse_localization pulse_user_storage)
    pulse_enable_lumatext(pulse)
    pulse_add_version_resource(pulse "Pulse file manager" "Pulse" "Pulse.exe")
endif()

# ---- Pulse.Index.exe: index + query host (session helper or Windows service) ----
add_executable(pulse_index WIN32
    src/index/index_host.cpp
    src/index/user_index_storage.cpp
    src/index/index_path_service.cpp
    src/index/index_migration.cpp
    src/index/network_agent_main.cpp
    src/index/content_agent.cpp
    src/index/content_instant_session.cpp
    src/index/content_search.cpp
    src/index/index_engine.cpp
    src/index/change_tracking.cpp
    src/index/index_visibility.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_mft.cpp
    src/index/index_paths.cpp
    src/index/index_delta.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp
    src/fs/fs_enum.cpp)
target_sources(pulse_index PRIVATE src/index/index_host.rc)
target_link_options(pulse_index PRIVATE /MANIFEST:NO)
set_target_properties(pulse_index PROPERTIES OUTPUT_NAME "Pulse.Index")
target_link_libraries(pulse_index PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr bcrypt
    pulse_crash_reporter pulse_text)
pulse_add_version_resource(pulse_index "Pulse index host" "Pulse.Index" "Pulse.Index.exe")
add_dependencies(pulse_index_host_stress pulse_index)

# ---- Stage 1B-1 ops self test (console; drives Pulse.Shell.exe over the pipe) ----
add_executable(pulse_network_security_test src/bench/network_security_test.cpp)
target_link_libraries(pulse_network_security_test PRIVATE advapi32)
add_dependencies(pulse_network_security_test pulse_index)
add_executable(pulse_index_security_test src/bench/index_security_test.cpp src/index/change_tracking.cpp)
target_link_libraries(pulse_index_security_test PRIVATE advapi32)
add_executable(pulse_worker_lifecycle_test
    src/bench/worker_lifecycle_test.cpp src/app/app_worker.cpp src/app/entry_sort.cpp
    src/app/link_resolve.cpp src/fs/fs_enum.cpp src/fs/fs_snapshot.cpp
    src/fs/fs_recycle.cpp src/fs/fs_net_cache.cpp)
target_link_libraries(pulse_worker_lifecycle_test PRIVATE pulse_localization ole32 shell32 uuid shlwapi netapi32 pulse_shell_query_client)
add_executable(pulse_fs_worker_cancel_test
    src/bench/fs_enumeration_worker_test.cpp src/app/app_worker.cpp src/app/entry_sort.cpp
    src/app/link_resolve.cpp src/fs/fs_enum.cpp src/fs/fs_snapshot.cpp
    src/fs/fs_recycle.cpp src/fs/fs_net_cache.cpp)
target_compile_definitions(pulse_fs_worker_cancel_test PRIVATE PULSE_WORKER_TESTING=1 PULSE_NET_CACHE_TESTING=1)
target_link_libraries(pulse_fs_worker_cancel_test PRIVATE pulse_localization ole32 shell32 uuid shlwapi netapi32 pulse_shell_query_client)
add_executable(pulse_fs_watch_lifecycle_test
    src/bench/fs_watch_lifecycle_test.cpp src/fs/fs_watch.cpp src/fs/fs_enum.cpp)
target_compile_definitions(pulse_fs_watch_lifecycle_test PRIVATE PULSE_FS_WATCH_TESTING=1)
target_link_libraries(pulse_fs_watch_lifecycle_test PRIVATE shell32 shlwapi netapi32)
add_executable(pulse_tag_discovery_cache_test src/bench/tag_discovery_cache_test.cpp)
add_executable(pulse_ipc_payload_test src/bench/ipc_payload_test.cpp)
add_executable(pulse_operation_presentation_test src/bench/operation_presentation_test.cpp)
add_executable(pulse_column_strip_test src/bench/column_strip_test.cpp src/ui/column_strip_layout.cpp)
add_executable(pulse_playback_controls_test src/bench/playback_controls_test.cpp src/ui/video_preview.cpp)
target_link_libraries(pulse_playback_controls_test PRIVATE user32 gdi32 ole32 mfuuid)
target_compile_definitions(pulse_playback_controls_test PRIVATE PULSE_VIDEO_PREVIEW_TESTING=1)
add_executable(pulse_gif_decoder_test src/bench/gif_decoder_test.cpp)
target_link_libraries(pulse_gif_decoder_test PRIVATE ole32 windowscodecs uuid)

add_executable(pulse_ops_test
    src/bench/ops_test_main.cpp
    src/bench/file_operation_matrix.cpp
    src/ops/ops_manager.cpp
    src/ops/clipboard.cpp
    src/shell_host/recycle_fileop.cpp
    src/shell_host/deletion_identity.cpp
    src/ipc/shell_client.cpp)
target_link_libraries(pulse_ops_test PRIVATE ole32 shell32 uuid bcrypt)

add_executable(pulse_drag_drop_test src/bench/drag_drop_test_main.cpp
    src/ui/drag_drop.cpp src/ops/clipboard.cpp)
target_compile_definitions(pulse_drag_drop_test PRIVATE PULSE_DRAG_DROP_TESTING=1)
target_link_libraries(pulse_drag_drop_test PRIVATE ole32 shell32 uuid advapi32 pulse_shell_query_client)
add_executable(pulse_drop_staging_test src/bench/drop_staging_test_main.cpp
    src/ops/drop_staging.cpp)
target_link_libraries(pulse_drop_staging_test PRIVATE user32)

add_executable(pulse_rename_ops_test src/bench/rename_ops_test.cpp
    src/ops/ops_manager.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_rename_ops_test PRIVATE ole32 shell32 uuid bcrypt)
add_executable(pulse_recycle_backend_test src/bench/recycle_backend_test.cpp
    src/ops/ops_manager.cpp src/ipc/shell_client.cpp src/shell_host/deletion_identity.cpp
    src/bench/localization_test.rc)
add_executable(pulse_audit_ops_followup_test src/bench/audit_ops_followup_test.cpp
    src/ops/ops_manager.cpp src/ipc/shell_client.cpp src/shell_host/deletion_identity.cpp
    src/ops/drop_staging.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_audit_ops_followup_test PRIVATE ole32 shell32 uuid bcrypt)
set_property(SOURCE src/app/pulse.rc src/bench/localization_test.rc
    src/shell_host/operation_worker.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc;${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_ids.h;${CMAKE_CURRENT_SOURCE_DIR}/src/common/runtime_messages.inc")
set_property(SOURCE src/app/pulse.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_BINARY_DIR}/generated/pulse_app.manifest")
set_property(SOURCE src/bench/dialog_edit_test.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_ids.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/runtime_messages.inc")
target_link_libraries(pulse_recycle_backend_test PRIVATE ole32 shell32 uuid bcrypt)
# Standalone OpsManager tests need the deletion service's direct dependencies.
foreach(ops_test_target pulse_ops_test pulse_rename_ops_test pulse_recycle_backend_test pulse_audit_ops_followup_test)
    target_sources(${ops_test_target} PRIVATE
        src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
        src/ops/recycle_undo_validation.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp)
    target_link_libraries(${ops_test_target} PRIVATE
        pulse_localization advapi32 shlwapi netapi32 mpr)
endforeach()
add_executable(pulse_delete_service_test src/bench/delete_service_test.cpp
    src/ops/delete_service.cpp)
add_executable(pulse_metafile_input_test src/bench/metafile_input_test.cpp src/preview_host/metafile_raster.cpp)
target_link_libraries(pulse_metafile_input_test PRIVATE ole32 gdi32 gdiplus)

add_executable(pulse_binary_input_boundary_test src/bench/binary_input_boundary_test.cpp)
add_executable(pulse_permanent_fileop_console_test src/bench/permanent_fileop_console_test.cpp)

# ---- Pulse.Shell.exe: COM isolation and independent Shell launch sessions ----
file(GLOB_RECURSE PULSE_SHELL_SOURCES CONFIGURE_DEPENDS src/shell_host/*.cpp)
if(PULSE_SHELL_SOURCES)
    add_executable(pulse_shell WIN32 ${PULSE_SHELL_SOURCES})
    target_sources(pulse_shell PRIVATE src/ops/image_open.cpp)
    target_sources(pulse_shell PRIVATE
        src/shell_host/operation_worker.rc
        src/ops/ops_manager.cpp src/ops/operation_permission.cpp
        src/ops/operation_elevation_adapter.cpp src/ops/operation_elevation_transport.cpp
        src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
        src/ops/recycle_undo_validation.cpp src/ipc/shell_client.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp)
    target_compile_definitions(pulse_shell PRIVATE PULSE_OPERATION_ELEVATION_HOST=1)
    set_target_properties(pulse_shell PROPERTIES OUTPUT_NAME "Pulse.Shell")
    target_link_libraries(pulse_shell PRIVATE ole32 shell32 uuid advapi32 shlwapi propsys version windowscodecs comctl32 pulse_crash_reporter)
    target_link_libraries(pulse_shell PRIVATE pulse_localization bcrypt netapi32 mpr rstrtmgr)
    pulse_add_version_resource(pulse_shell "Pulse Shell host" "Pulse.Shell" "Pulse.Shell.exe")
    add_executable(pulse_context_worker_fixture WIN32 ${PULSE_SHELL_SOURCES})
    target_sources(pulse_context_worker_fixture PRIVATE src/ops/image_open.cpp)
    target_compile_definitions(pulse_context_worker_fixture PRIVATE PULSE_CONTEXT_PROCESS_TESTING=1)
    target_link_libraries(pulse_context_worker_fixture PRIVATE
        ole32 shell32 uuid advapi32 shlwapi propsys version windowscodecs comctl32 pulse_crash_reporter)
    add_executable(pulse_context_session_test src/bench/context_session_process_test.cpp)
    add_dependencies(pulse_context_session_test pulse_context_worker_fixture)
    add_executable(pulse_shell_launch_worker_fixture WIN32 ${PULSE_SHELL_SOURCES} src/ops/image_open.cpp)
    target_compile_definitions(pulse_shell_launch_worker_fixture PRIVATE PULSE_SHELL_LAUNCH_TESTING=1)
    target_link_libraries(pulse_shell_launch_worker_fixture PRIVATE
        ole32 shell32 uuid advapi32 shlwapi propsys version windowscodecs comctl32 pulse_crash_reporter)
    add_executable(pulse_shell_launch_test src/bench/shell_launch_test.cpp)
    target_link_libraries(pulse_shell_launch_test PRIVATE user32)
    add_dependencies(pulse_shell_launch_test pulse_shell_launch_worker_fixture)
    add_executable(pulse_shell_launch_client_test
        src/bench/shell_launch_client_test.cpp src/ipc/shell_client.cpp)
    target_link_libraries(pulse_shell_launch_client_test PRIVATE advapi32 user32)
    add_executable(pulse_shell_interaction_test src/bench/shell_interaction_test.cpp)
    target_link_libraries(pulse_shell_interaction_test PRIVATE pulse_shell_interaction_client)
    add_dependencies(pulse_shell_interaction_test pulse_shell)
    add_executable(pulse_shell_query_fixture WIN32 ${PULSE_SHELL_SOURCES} src/ops/image_open.cpp)
    target_compile_definitions(pulse_shell_query_fixture PRIVATE PULSE_SHELL_QUERY_TESTING=1)
    target_link_libraries(pulse_shell_query_fixture PRIVATE
        ole32 shell32 uuid advapi32 shlwapi propsys version windowscodecs comctl32 pulse_crash_reporter)
    add_executable(pulse_shell_query_test src/bench/shell_query_test.cpp
        src/shell_host/shell_query_metadata.cpp)
    target_link_libraries(pulse_shell_query_test PRIVATE ole32 shell32 uuid advapi32 user32 shlwapi)
    add_dependencies(pulse_shell_query_test pulse_shell_query_fixture)
    foreach(shell_host_target pulse_shell pulse_context_worker_fixture
            pulse_shell_launch_worker_fixture pulse_shell_query_fixture)
        target_sources(${shell_host_target} PRIVATE src/shell_host/shell_host.rc)
        target_link_options(${shell_host_target} PRIVATE /MANIFEST:NO)
    endforeach()
    set_property(SOURCE src/shell_host/shell_host.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shell_host/shell_host.manifest")
endif()

add_executable(pulse_shell_request_ownership_test EXCLUDE_FROM_ALL
    src/bench/shell_request_ownership_test.cpp src/shell_host/deletion_identity.cpp)
target_compile_options(pulse_shell_request_ownership_test PRIVATE /Gy /Gw)
target_link_options(pulse_shell_request_ownership_test PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(pulse_shell_request_ownership_test PRIVATE ole32 shell32 uuid advapi32 user32 shlwapi)

# ---- Pulse.Preview.exe: thumbnail decoding / Shell provider isolation ----
add_executable(pulse_preview WIN32 src/preview_host/main.cpp src/preview_host/video_codec.cpp
    src/preview_host/svg_raster.cpp src/preview_host/metafile_raster.cpp
    src/preview_host/jpeg_embedded_preview.cpp src/preview_host/image_decoder.cpp)
set_target_properties(pulse_preview PROPERTIES OUTPUT_NAME "Pulse.Preview")
# gdiplus renders WMF/EMF, including the EMF+ records Office clip art uses.
target_link_libraries(pulse_preview PRIVATE ole32 shell32 gdi32 uuid windowscodecs propsys advapi32 gdiplus)
# SVG previews are rasterized with Direct2D, but d2d1/d3d11/dxgi are
# deliberately not linked: implicit imports would load them at every preview
# host start-up (the first preview of any file type) while only SVG needs them.
# svg_raster.cpp resolves them on demand from System32 instead.
target_link_libraries(pulse_preview PRIVATE pulse_crash_reporter)
pulse_add_version_resource(pulse_preview "Pulse preview host" "Pulse.Preview" "Pulse.Preview.exe")

# Preview protocol self test (console; drives Pulse.Preview.exe over the pipe)
add_executable(pulse_preview_test src/bench/preview_test_main.cpp
    src/bench/thumbnail_cache_test.cpp src/ui/thumbnail_cache.cpp src/ui/thumbnail_disk_cache.cpp
    src/preview_host/video_codec.cpp)
target_link_libraries(pulse_preview_test PRIVATE ole32 gdi32 gdiplus shell32 windowscodecs uuid)
add_dependencies(pulse_preview_test pulse_preview)

add_executable(pulse_jpeg_embedded_preview_test EXCLUDE_FROM_ALL
    src/bench/jpeg_embedded_preview_test.cpp src/preview_host/jpeg_embedded_preview.cpp
    src/preview_host/image_decoder.cpp)
target_link_libraries(pulse_jpeg_embedded_preview_test PRIVATE ole32 windowscodecs uuid)

add_executable(pulse_shell_icons_test src/bench/shell_icons_test.cpp src/ui/shell_icons.cpp
    src/shell_host/shell_query_icons.cpp)
target_compile_definitions(pulse_shell_icons_test PRIVATE PULSE_SHELL_ICONS_TESTING=1)
target_link_libraries(pulse_shell_icons_test PRIVATE ole32 shell32 comctl32 uuid windowscodecs pulse_shell_query_client)
add_dependencies(pulse_shell_icons_test pulse_shell)
add_executable(pulse_link_query_test src/bench/link_query_test.cpp src/app/link_resolve.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_link_query_test PRIVATE shell32 shlwapi netapi32)
add_executable(pulse_header_drop_effect_test src/bench/header_drop_effect_test.cpp)

add_executable(pulse_preview_handler_probe
    src/bench/preview_handler_probe.cpp
    src/ui/preview_handler_host.cpp)
target_compile_definitions(pulse_preview_handler_probe PRIVATE
    PULSE_PREVIEW_HANDLER_TESTING=1)
target_link_libraries(pulse_preview_handler_probe PRIVATE
    ole32 shell32 uuid shlwapi oleaut32 d2d1 gdi32 user32)

add_executable(pulse_preview_handler_pan_test src/bench/preview_handler_pan_test.cpp)
target_compile_definitions(pulse_preview_handler_pan_test PRIVATE PULSE_PREVIEW_PAN_TESTING=1)
target_link_libraries(pulse_preview_handler_pan_test PRIVATE gdi32 user32)

add_executable(pulse_material_test
    src/bench/window_material_test.cpp
    src/ui/window_material.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp src/ui/dialog_lifecycle.cpp)
target_compile_definitions(pulse_material_test PRIVATE
    PULSE_WINDOW_MATERIAL_TESTING=1)
target_link_libraries(pulse_material_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32
    comctl32 pulse_localization)
pulse_enable_lumatext(pulse_material_test)

add_executable(pulse_settings_value_ui_test
    src/bench/settings_value_ui_test.cpp src/bench/localization_test.rc
    src/app/settings_value_dialog.cpp src/ui/dialog_lifecycle.cpp)
target_link_libraries(pulse_settings_value_ui_test PRIVATE
    pulse_localization user32 gdi32 comctl32 uxtheme dwmapi dcomp ole32 windowscodecs shcore)

add_executable(pulse_image_open_test src/bench/image_open_test.cpp)
target_link_libraries(pulse_image_open_test PRIVATE
    ole32 shell32 shlwapi propsys uuid user32)

add_executable(pulse_search_history_test
    src/bench/search_history_test.cpp
    src/app/search_history.cpp)

add_executable(pulse_app_controllers_test
    src/bench/app_controllers_test.cpp
    src/app/context_menu.cpp src/app/command_catalog.cpp
    src/bench/localization_test.rc
    src/app/settings_controller.cpp
    src/app/system_ctx_model.cpp src/app/system_ctx_registry.cpp
    src/app/context_menu_controller.cpp
    src/shell_host/ctx_handlers.cpp
    src/app/session_tab_model.cpp
    src/app/tab_group_model.cpp
    src/app/single_instance_coordinator.cpp
    src/app/tray_controller.cpp
    src/app/app_prefs.cpp src/app/startup_launch.cpp src/app/default_file_manager.cpp
    src/app/folder_view_prefs.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp src/model/builtin_menu_items.cpp
    src/ui/view_layout.cpp
    src/index/index_client.cpp
    src/index/index_config.cpp
    src/index/network_agent_client.cpp
    src/index/index_query.cpp src/index/search_result_merge.cpp src/index/search_kinds.cpp)
target_link_libraries(pulse_app_controllers_test PRIVATE
    user32 shell32 ole32 uuid advapi32 shlwapi version pulse_localization pulse_user_storage)

add_executable(pulse_command_line_open_test src/bench/command_line_open_test.cpp
    src/app/startup_launch.cpp src/app/single_instance_coordinator.cpp src/app/default_file_manager.cpp)
target_link_libraries(pulse_command_line_open_test PRIVATE user32 shell32 advapi32)

add_executable(pulse_shell_launch_regression_test src/bench/shell_launch_regression_test.cpp
    src/app/context_menu_controller.cpp src/app/context_menu_prefs.cpp
    src/app/builtin_menu_items.cpp
    src/model/builtin_menu_items.cpp src/ops/image_open.cpp src/shell_host/shell_interaction_worker.cpp)
target_compile_options(pulse_shell_launch_regression_test PRIVATE /Gy)
target_link_options(pulse_shell_launch_regression_test PRIVATE /OPT:REF)
target_link_libraries(pulse_shell_launch_regression_test PRIVATE pulse_localization pulse_user_storage
    user32 shell32 ole32 oleaut32 uuid advapi32 shlwapi propsys comdlg32)

# Shell-only operations (recycle bin, Explorer verbs) go through Pulse.Shell.exe.
# Copy/move use the in-process scheduler plus CopyFile2.
# Filename search goes through Pulse.Index.exe (never Engine::Start in the UI).
if(TARGET pulse AND TARGET pulse_shell)
    add_dependencies(pulse pulse_shell)
endif()
if(TARGET pulse AND TARGET pulse_preview)
    add_dependencies(pulse pulse_preview)
endif()
if(TARGET pulse AND TARGET pulse_index)
    add_dependencies(pulse pulse_index)
endif()
if(TARGET pulse_ops_test AND TARGET pulse_shell)
    add_dependencies(pulse_ops_test pulse_shell)
    add_dependencies(pulse_rename_ops_test pulse_shell)
    add_dependencies(pulse_recycle_backend_test pulse_shell)
endif()

add_executable(pulse_link_confirmation_ui_test
    src/bench/link_confirmation_ui_test.cpp src/bench/localization_test.rc
    src/ui/file_operation_dialog.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp
    src/ops/file_lock_owner.cpp)
target_compile_definitions(pulse_link_confirmation_ui_test PRIVATE PULSE_UI_TEST_HOOKS=1)
target_link_libraries(pulse_link_confirmation_ui_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32)
pulse_enable_lumatext(pulse_link_confirmation_ui_test)

# Headless modal lifecycle checks: no screenshots and no foreground activation.
add_executable(pulse_dialog_close_test src/bench/dialog_close_test.cpp src/ui/dialog_lifecycle.cpp)
target_compile_features(pulse_dialog_close_test PRIVATE cxx_std_20)
target_compile_options(pulse_dialog_close_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_dialog_close_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(pulse_dialog_close_test PRIVATE dwmapi user32 comctl32)

add_executable(pulse_dialog_fade_test
    src/bench/dialog_fade_test.cpp src/bench/localization_test.rc
    src/app/new_item_dialog.cpp src/app/archive_dialog.cpp
    src/model/batch_rename.cpp src/fs/fs_enum.cpp
    src/ui/dialog_lifecycle.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_include_directories(pulse_dialog_fade_test PRIVATE src)
target_link_libraries(pulse_dialog_fade_test PRIVATE
    pulse_localization comctl32 user32 gdi32 dwmapi dcomp d3d11 d2d1 dwrite dxgi
    shcore ole32 shell32 uuid windowscodecs netapi32 shlwapi)
pulse_enable_lumatext(pulse_dialog_fade_test)

add_executable(pulse_child_edit_test
    src/bench/child_edit_test.cpp src/bench/child_edit_test.rc
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/dialog_lifecycle.cpp src/ui/edit_host.cpp)
target_compile_options(pulse_child_edit_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_child_edit_test PRIVATE PULSE_TEST_LUMATEXT_INIT_FAILURE=1 PULSE_TEST_EDIT_VIEWPORT=1)
target_link_options(pulse_child_edit_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_child_edit_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_child_edit_test)

add_executable(pulse_sidebar_scrollbar_fade_test src/bench/sidebar_scrollbar_fade_test.cpp)
target_compile_options(pulse_sidebar_scrollbar_fade_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_builtin_menu_items_test
    src/bench/builtin_menu_items_test.cpp src/bench/localization_test.rc
    src/app/builtin_menu_items.cpp src/model/builtin_menu_items.cpp src/app/context_menu_prefs.cpp)
target_compile_options(pulse_builtin_menu_items_test PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_builtin_menu_items_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_builtin_menu_items_test PRIVATE pulse_localization)

add_executable(pulse_builtin_menu_order_test
    src/bench/builtin_menu_order_test.cpp src/bench/localization_test.rc
    src/app/builtin_menu_items.cpp src/model/builtin_menu_items.cpp src/app/context_menu_prefs.cpp)
target_compile_options(pulse_builtin_menu_order_test PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_builtin_menu_order_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_builtin_menu_order_test PRIVATE pulse_localization)

add_executable(pulse_integration src/integration_host/main.cpp src/app/system_ctx_registry.cpp)
set_target_properties(pulse_integration PROPERTIES OUTPUT_NAME "Pulse.Integration")
pulse_add_version_resource(pulse_integration "Pulse context menu integration helper" "Pulse.Integration" "Pulse.Integration.exe")
target_compile_options(pulse_integration PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_integration PRIVATE advapi32 shell32 shlwapi ole32 version)
add_dependencies(pulse pulse_integration)

add_executable(pulse_system_ctx_test src/bench/system_ctx_test.cpp src/app/system_ctx_registry.cpp)
add_executable(pulse_system_ctx_exchange_test src/bench/system_ctx_exchange_test.cpp)
target_compile_options(pulse_system_ctx_exchange_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_system_ctx_exchange_test PRIVATE advapi32)
add_executable(pulse_system_ctx_transaction_console_test
    src/bench/system_ctx_transaction_console_test.cpp src/app/system_ctx_registry.cpp)
target_compile_options(pulse_system_ctx_transaction_console_test PRIVATE /W4 /permissive- /utf-8 /Gy)
target_link_options(pulse_system_ctx_transaction_console_test PRIVATE /OPT:REF)
target_link_libraries(pulse_system_ctx_transaction_console_test PRIVATE advapi32 shell32 shlwapi ole32 version)
add_executable(pulse_system_ctx_model_test src/bench/system_ctx_model_test.cpp
    src/app/system_ctx_registry.cpp src/app/system_ctx_model.cpp src/app/context_menu_prefs.cpp
    src/model/builtin_menu_items.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_system_ctx_model_test PRIVATE pulse_user_storage pulse_localization)
foreach(context_target pulse_system_ctx_test pulse_system_ctx_model_test)
    target_compile_options(${context_target} PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(${context_target} PRIVATE advapi32 shell32 shlwapi ole32 version)
endforeach()

foreach(model_fixture IN ITEMS quick_access_badges entry_order_hold drive_view breadcrumb staging_tray pane_filter pane_animation folder_grouping)
    add_executable(pulse_${model_fixture}_test
        src/bench/${model_fixture}_test.cpp src/bench/localization_test.rc
        src/app/app_model.cpp src/app/places.cpp src/app/entry_sort.cpp
        src/app/entry_order_hold.cpp src/app/snapshot_patch.cpp src/app/link_resolve.cpp
        src/app/date_groups.cpp src/ui/view_layout.cpp src/model/search_query.cpp
        src/app/column_view_model.cpp src/index/index_query.cpp src/index/search_kinds.cpp
        src/fs/fs_enum.cpp src/index/content_result_store.cpp src/model/item_filter.cpp src/index/filename_pinyin.cpp)
    target_sources(pulse_${model_fixture}_test PRIVATE src/app/cloud_roots.cpp src/app/cloud_root_catalog.cpp)
    target_compile_options(pulse_${model_fixture}_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(pulse_${model_fixture}_test PRIVATE /MANIFEST:NO)
    target_link_libraries(pulse_${model_fixture}_test PRIVATE
        pulse_sqlite pulse_localization pulse_text pulse_shell_query_client shell32 ole32 uuid shlwapi netapi32 bcrypt secur32)
endforeach()
get_target_property(controller_model_sources pulse_quick_access_badges_test SOURCES)
list(FILTER controller_model_sources EXCLUDE REGEX "(quick_access_badges_test\\.cpp|localization_test\\.rc|view_layout\\.cpp|index_query\\.cpp|search_kinds\\.cpp)$")
target_sources(pulse_app_controllers_test PRIVATE ${controller_model_sources}
    src/app/cloud_roots.cpp src/app/cloud_root_catalog.cpp)
target_link_libraries(pulse_app_controllers_test PRIVATE pulse_sqlite pulse_text pulse_shell_query_client secur32 bcrypt netapi32)
target_sources(pulse_folder_grouping_test PRIVATE src/app/folder_view_prefs.cpp)
target_sources(pulse_drive_view_test PRIVATE src/app/context_menu.cpp src/app/command_catalog.cpp)

get_target_property(command_catalog_model_sources pulse_drive_view_test SOURCES)
list(FILTER command_catalog_model_sources EXCLUDE REGEX "drive_view_test\\.cpp$")
add_executable(pulse_command_catalog_test src/bench/command_catalog_test.cpp ${command_catalog_model_sources})
target_link_options(pulse_command_catalog_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_command_catalog_test PRIVATE pulse_sqlite pulse_localization pulse_text pulse_shell_query_client
    shell32 ole32 uuid shlwapi netapi32 bcrypt secur32)

add_executable(pulse_item_filter_test src/bench/item_filter_test.cpp src/model/item_filter.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_item_filter_test PRIVATE pulse_text)

get_target_property(text_compare_model_sources pulse_drive_view_test SOURCES)
list(FILTER text_compare_model_sources EXCLUDE REGEX "drive_view_test\\.cpp$")
target_sources(pulse_text_compare_test PRIVATE ${text_compare_model_sources})
target_link_libraries(pulse_text_compare_test PRIVATE
    pulse_sqlite pulse_localization pulse_shell_query_client shell32 ole32 uuid shlwapi netapi32 bcrypt secur32)

add_executable(pulse_smooth_scroll_test src/bench/smooth_scroll_test.cpp)

add_executable(pulse_text_diff_ui_test src/bench/text_diff_ui_test.cpp
    src/bench/localization_test.rc src/app/text_diff.cpp src/ui/text_diff_window.cpp
    src/ui/ui_compositor.cpp src/ui/fluent_components.cpp src/ui/typography.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp)
target_link_libraries(pulse_text_diff_ui_test PRIVATE pulse_text pulse_localization
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32 comctl32)
pulse_enable_lumatext(pulse_text_diff_ui_test)
target_sources(pulse_breadcrumb_test PRIVATE src/ui/breadcrumb.cpp)
target_sources(pulse_staging_tray_test PRIVATE src/app/tray_metadata.cpp src/app/app_worker.cpp
    src/fs/fs_snapshot.cpp src/fs/fs_recycle.cpp src/fs/fs_net_cache.cpp)
target_link_libraries(pulse_staging_tray_test PRIVATE pulse_shell_query_client)
add_executable(pulse_json_utils_test src/bench/json_utils_test.cpp)
add_executable(pulse_utf8_atomic_test src/bench/utf8_atomic_test.cpp)
get_target_property(json_model_sources pulse_quick_access_badges_test SOURCES)
list(FILTER json_model_sources EXCLUDE REGEX "quick_access_badges_test\\.cpp$")
add_executable(pulse_json_persistence_test src/bench/json_persistence_test.cpp ${json_model_sources}
    src/app/session.cpp src/app/session_tab_model.cpp src/app/tab_group_model.cpp
    src/app/app_prefs.cpp src/app/startup_launch.cpp src/app/default_file_manager.cpp
    src/app/folder_view_prefs.cpp src/app/context_menu_prefs.cpp
    src/app/builtin_menu_items.cpp src/model/builtin_menu_items.cpp)
target_link_libraries(pulse_json_persistence_test PRIVATE
    pulse_sqlite pulse_localization pulse_text pulse_user_storage pulse_shell_query_client
    shell32 ole32 uuid shlwapi netapi32 bcrypt secur32 advapi32 user32)
get_target_property(session_windows_sources pulse_json_persistence_test SOURCES)
list(FILTER session_windows_sources EXCLUDE REGEX "json_persistence_test\\.cpp$")
add_executable(pulse_session_windows_test src/bench/session_windows_test.cpp ${session_windows_sources})
get_target_property(session_windows_libraries pulse_json_persistence_test LINK_LIBRARIES)
target_link_libraries(pulse_session_windows_test PRIVATE ${session_windows_libraries})

add_executable(pulse_session_layout_test src/bench/session_layout_test.cpp ${json_model_sources}
    src/app/session.cpp src/app/session_tab_model.cpp src/app/tab_group_model.cpp)
target_compile_options(pulse_session_layout_test PRIVATE /Gy /Gw)
target_link_options(pulse_session_layout_test PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(pulse_session_layout_test PRIVATE
    pulse_sqlite pulse_localization pulse_text pulse_user_storage pulse_shell_query_client shell32 ole32 uuid shlwapi netapi32 bcrypt secur32)

add_executable(pulse_snapshot_patch_link_test EXCLUDE_FROM_ALL
    src/bench/snapshot_patch_link_test.cpp src/app/snapshot_patch.cpp
    src/app/link_resolve.cpp src/fs/fs_enum.cpp)
target_compile_options(pulse_snapshot_patch_link_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_snapshot_patch_link_test PRIVATE netapi32)

add_executable(pulse_app_integrations_lifecycle_test EXCLUDE_FROM_ALL
    src/bench/app_integrations_lifecycle_test.cpp src/app/device_registry.cpp
    src/fs/device_path.cpp src/fs/fs_enum.cpp)
target_compile_options(pulse_app_integrations_lifecycle_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_app_integrations_lifecycle_test PRIVATE user32 netapi32)
target_sources(pulse_ops_test PRIVATE src/bench/json_undo_parser_test.cpp)

add_executable(pulse_file_lock_owner_test
    src/bench/file_lock_owner_test.cpp src/ops/file_lock_owner.cpp)
target_compile_options(pulse_file_lock_owner_test PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_file_lock_owner_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_file_lock_owner_test PRIVATE rstrtmgr advapi32)

add_executable(pulse_default_file_manager_test
    src/bench/default_file_manager_test.cpp src/app/default_file_manager.cpp
    src/app/shell_window_registry.cpp src/app/explorer_window_takeover.cpp)
target_compile_options(pulse_default_file_manager_test PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_default_file_manager_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_default_file_manager_test PRIVATE
    advapi32 shell32 ole32 oleaut32 uuid propsys user32 shlwapi pulse_shell_query_client)

add_executable(pulse_explorer_takeover_test
    src/bench/explorer_takeover_test.cpp src/app/explorer_window_takeover.cpp
    src/app/default_file_manager.cpp)
target_link_options(pulse_explorer_takeover_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_explorer_takeover_test PRIVATE
    advapi32 shell32 ole32 oleaut32 uuid user32 shlwapi)
add_executable(pulse_shell_registry_ipc_test
    src/bench/shell_registry_ipc_test.cpp src/app/shell_window_registry.cpp)
target_link_libraries(pulse_shell_registry_ipc_test PRIVATE
    pulse_shell_query_client shell32 ole32 oleaut32 uuid propsys user32 shlwapi)
add_dependencies(pulse_shell_registry_ipc_test pulse_shell)

add_executable(pulse_dialog_text_fit_test src/bench/dialog_text_fit_test.cpp)
add_executable(pulse_clipboard_availability_test src/bench/clipboard_availability_test.cpp)
target_compile_options(pulse_clipboard_availability_test PRIVATE /W4 /permissive- /utf-8)
add_executable(pulse_virtual_clipboard_test src/bench/virtual_clipboard_test.cpp
    src/ops/clipboard.cpp)
target_compile_definitions(pulse_virtual_clipboard_test PRIVATE PULSE_CLIPBOARD_TESTING=1)
target_link_libraries(pulse_virtual_clipboard_test PRIVATE shell32 ole32 oleaut32 uuid user32 shlwapi)
add_executable(pulse_window_result_mailbox_test src/bench/window_result_mailbox_test.cpp)
target_compile_options(pulse_window_result_mailbox_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_window_result_mailbox_test PRIVATE user32)
add_executable(pulse_startup_command_test src/bench/startup_command_test.cpp src/app/startup_launch.cpp)

add_executable(pulse_locked_operation_test
    src/bench/locked_operation_test.cpp src/ops/ops_manager.cpp
    src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
    src/ops/recycle_undo_validation.cpp src/ipc/shell_client.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp)
target_link_libraries(pulse_locked_operation_test PRIVATE
    pulse_localization ole32 shell32 uuid bcrypt advapi32 shlwapi netapi32 mpr rstrtmgr)

add_executable(pulse_operation_records_test
    src/bench/operation_records_test.cpp src/ops/ops_manager.cpp
    src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
    src/ops/recycle_undo_validation.cpp src/ipc/shell_client.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp)
target_link_libraries(pulse_operation_records_test PRIVATE
    pulse_localization ole32 shell32 uuid bcrypt advapi32 shlwapi netapi32 mpr rstrtmgr)

foreach(elevation_ops_target pulse_ops_test pulse_rename_ops_test pulse_recycle_backend_test pulse_audit_ops_followup_test pulse_locked_operation_test pulse_operation_records_test)
    target_sources(${elevation_ops_target} PRIVATE
        src/ops/operation_permission.cpp src/ops/operation_elevation_adapter.cpp
        src/ops/operation_elevation_transport.cpp)
endforeach()

add_executable(pulse_operation_elevation_adapter_test
    src/bench/operation_elevation_adapter_test.cpp src/bench/localization_test.rc
    src/ops/ops_manager.cpp src/ops/operation_permission.cpp
    src/ops/operation_elevation_adapter.cpp src/ops/operation_elevation_transport.cpp
    src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
    src/ops/recycle_undo_validation.cpp src/ipc/shell_client.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp)
target_compile_definitions(pulse_operation_elevation_adapter_test PRIVATE PULSE_OPERATION_ELEVATION_TESTING)
target_link_libraries(pulse_operation_elevation_adapter_test PRIVATE
    pulse_localization ole32 shell32 uuid bcrypt advapi32 shlwapi netapi32 mpr rstrtmgr)
add_dependencies(pulse_operation_elevation_adapter_test pulse_shell)

add_executable(pulse_operation_elevation_transport_test
    src/bench/operation_elevation_transport_test.cpp src/ops/operation_elevation_transport.cpp)
target_compile_definitions(pulse_operation_elevation_transport_test PRIVATE PULSE_OPERATION_ELEVATION_TESTING)
target_link_libraries(pulse_operation_elevation_transport_test PRIVATE shell32 advapi32 bcrypt)

add_executable(pulse_ops_feedback_test
    src/bench/ops_feedback_test.cpp src/bench/localization_test.rc src/ops/ops_manager.cpp
    src/ops/operation_permission.cpp src/ops/operation_elevation_adapter.cpp src/ops/operation_elevation_transport.cpp
    src/ops/delete_service.cpp src/ops/delete_operation.cpp src/ops/file_lock_owner.cpp
    src/ops/recycle_undo_validation.cpp src/ipc/shell_client.cpp src/fs/fs_enum.cpp src/fs/fs_recycle.cpp
    src/shell_host/recycle_fileop.cpp src/shell_host/deletion_identity.cpp)
target_link_options(pulse_ops_feedback_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_ops_feedback_test PRIVATE
    pulse_localization ole32 shell32 uuid bcrypt advapi32 shlwapi netapi32 mpr rstrtmgr)

add_executable(pulse_menu_text_test
    src/bench/menu_text_test.cpp src/bench/localization_test.rc
    src/ui/window_material.cpp
    src/ui/fluent_menu.cpp src/ui/fluent_components.cpp src/ui/ui_compositor.cpp src/ui/dialog_lifecycle.cpp
    src/ui/tab_shape.cpp src/ui/typography.cpp src/ui/edit_host.cpp)
target_link_options(pulse_menu_text_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_menu_text_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32)
pulse_enable_lumatext(pulse_menu_text_test)

add_executable(pulse_context_menu_loading_test
    src/bench/context_menu_loading_test.cpp src/bench/localization_test.rc
    src/app/context_menu_controller.cpp src/app/context_menu_prefs.cpp
    src/app/builtin_menu_items.cpp
    src/model/builtin_menu_items.cpp src/ipc/shell_client.cpp
    src/ui/window_material.cpp src/ui/fluent_menu.cpp src/ui/fluent_components.cpp
    src/ui/ui_compositor.cpp src/ui/dialog_lifecycle.cpp src/ui/tab_shape.cpp
    src/ui/typography.cpp src/ui/edit_host.cpp)
target_link_options(pulse_context_menu_loading_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_context_menu_loading_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 shlwapi)
pulse_enable_lumatext(pulse_context_menu_loading_test)
add_dependencies(pulse_context_menu_loading_test pulse_shell)

add_executable(pulse_context_query_timeout_test
    src/bench/context_query_timeout_test.cpp src/shell_host/ctx_session_process.cpp)
target_link_libraries(pulse_context_query_timeout_test PRIVATE user32)

add_executable(pulse_locked_item_prompt_ui_test
    src/bench/locked_item_prompt_ui_test.cpp src/bench/localization_test.rc
    src/ui/file_operation_dialog.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ops/file_lock_owner.cpp)
target_compile_definitions(pulse_locked_item_prompt_ui_test PRIVATE PULSE_UI_TEST_HOOKS=1)
target_link_options(pulse_locked_item_prompt_ui_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_locked_item_prompt_ui_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 rstrtmgr)
pulse_enable_lumatext(pulse_locked_item_prompt_ui_test)

add_executable(pulse_file_operation_failure_test
    src/bench/file_operation_failure_test.cpp src/bench/localization_test.rc
    src/app/archive_operation_ui.cpp
    src/ui/file_operation_dialog.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ops/file_lock_owner.cpp)
target_compile_definitions(pulse_file_operation_failure_test PRIVATE PULSE_UI_TEST_HOOKS=1)
target_link_options(pulse_file_operation_failure_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_file_operation_failure_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 rstrtmgr)
pulse_enable_lumatext(pulse_file_operation_failure_test)

add_executable(pulse_operation_center_ui_test
    src/bench/operation_center_ui_test.cpp src/bench/localization_test.rc
    src/ui/operation_center.cpp src/ui/operation_center_animation.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_link_options(pulse_operation_center_ui_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_operation_center_ui_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32)
pulse_enable_lumatext(pulse_operation_center_ui_test)

get_target_property(batch_conflict_ui_sources pulse_operation_center_ui_test SOURCES)
list(FILTER batch_conflict_ui_sources EXCLUDE REGEX "operation_center(_animation|_ui_test)?\\.cpp$")
add_executable(pulse_batch_conflict_dialog_test src/bench/batch_conflict_dialog_test.cpp
    src/ui/batch_conflict_dialog.cpp ${batch_conflict_ui_sources})
target_link_options(pulse_batch_conflict_dialog_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_batch_conflict_dialog_test PRIVATE pulse_localization
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid comctl32 user32 gdi32 windowscodecs advapi32)
pulse_enable_lumatext(pulse_batch_conflict_dialog_test)

add_executable(pulse_rename_locked_prompt_ui_test
    src/bench/rename_locked_prompt_ui_test.cpp src/bench/localization_test.rc
    src/ui/file_operation_dialog.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ops/file_lock_owner.cpp)
target_compile_definitions(pulse_rename_locked_prompt_ui_test PRIVATE PULSE_UI_TEST_HOOKS=1)
target_link_options(pulse_rename_locked_prompt_ui_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_rename_locked_prompt_ui_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 rstrtmgr)
pulse_enable_lumatext(pulse_rename_locked_prompt_ui_test)

add_executable(pulse_dialog_edit_integration_test
    src/bench/dialog_edit_integration_test.cpp src/bench/dialog_edit_test.rc
    src/ui/advanced_search_dialog.cpp src/ui/batch_rename_dialog.cpp
    src/model/batch_rename.cpp src/model/search_query.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/fs/fs_enum.cpp src/ui/edit_host.cpp src/ui/fluent_menu.cpp
    src/ui/fluent_components.cpp src/ui/window_helpers.cpp src/ui/tab_shape.cpp
    src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_compile_definitions(pulse_dialog_edit_integration_test PRIVATE PULSE_TEST_LUMATEXT_INIT_FAILURE=1)
target_link_options(pulse_dialog_edit_integration_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_dialog_edit_integration_test PRIVATE
    pulse_localization d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 shlwapi netapi32)
pulse_enable_lumatext(pulse_dialog_edit_integration_test)

# Focused folder change tracking regression tests.
add_executable(pulse_change_tracking_polling_test src/bench/change_tracking_polling_test.cpp)
target_link_libraries(pulse_change_tracking_polling_test PRIVATE user32)
add_executable(pulse_change_tracking_test src/bench/change_tracking_test.cpp src/index/change_tracking.cpp)
add_executable(pulse_change_tracking_memory_test src/bench/change_tracking_memory_test.cpp)
add_executable(pulse_change_feed_memory_test src/bench/change_feed_memory_test.cpp)
add_executable(pulse_usn_packet_queue_test src/bench/usn_packet_queue_test.cpp)
add_executable(pulse_change_tracking_client_test src/bench/change_tracking_client_test.cpp)
add_executable(pulse_global_search_hotkey_test src/bench/global_search_hotkey_test.cpp src/app/global_search_hotkey.cpp)
target_link_libraries(pulse_global_search_hotkey_test PRIVATE user32)

# Search enhancement: native pinyin data and per-user full-text index.
add_library(pulse_sqlite STATIC third_party/sqlite/sqlite3.c)
# Mapping remains disabled by default. Only the short-lived task snapshot opts
# into a RAM-budgeted mapping; the 2 GiB default cap cannot cover large caches.
target_compile_definitions(pulse_sqlite PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_MAX_MMAP_SIZE=17179869184LL)
target_include_directories(pulse_sqlite PUBLIC third_party/sqlite)
set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "/W0;/TC")

add_executable(pulse_filename_pinyin_test
    src/bench/filename_pinyin_test.cpp src/index/index_query.cpp src/index/search_kinds.cpp)
add_executable(pulse_index_pinyin_test
    src/bench/index_pinyin_test.cpp src/index/index_engine.cpp
    src/index/change_tracking.cpp src/index/index_visibility.cpp src/index/index_config.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/index_mft.cpp
    src/index/index_paths.cpp src/index/index_delta.cpp src/index/index_shard.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_index_pinyin_test PRIVATE pulse_user_storage advapi32 shlwapi ole32 shell32 netapi32 mpr)
foreach(folder_size_target pulse_index pulse_index_engine_test pulse_index_pinyin_test pulse_index_storage_test pulse_index_compaction_test pulse_index_search_cache_test)
    target_sources(${folder_size_target} PRIVATE src/index/index_folder_sizes.cpp)
endforeach()

foreach(search_target pulse pulse_index pulse_content_search_test)
    if(NOT search_target STREQUAL "pulse")
        target_sources(${search_target} PRIVATE src/index/content_index.cpp)
    endif()
    target_link_libraries(${search_target} PRIVATE pulse_sqlite)
endforeach()
target_sources(pulse_content_search_test PRIVATE src/index/index_query.cpp src/index/search_kinds.cpp)
get_property(search_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(search_target IN LISTS search_targets)
    get_target_property(search_sources ${search_target} SOURCES)
    if("${search_sources}" MATCHES "index_query\\.cpp" AND NOT "${search_sources}" MATCHES "filename_pinyin\\.cpp")
        target_sources(${search_target} PRIVATE src/index/filename_pinyin.cpp)
    endif()
    if("${search_sources}" MATCHES "index_engine\\.cpp")
        if(NOT "${search_sources}" MATCHES "index_engine_pinyin\\.cpp")
            target_sources(${search_target} PRIVATE src/index/index_engine_pinyin.cpp)
            target_include_directories(${search_target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
        endif()
        if(NOT "${search_sources}" MATCHES "filename_timing\\.cpp")
            target_sources(${search_target} PRIVATE src/index/filename_timing.cpp)
        endif()
    endif()
endforeach()
add_executable(pulse_content_index_test src/bench/content_index_test.cpp src/bench/content_instant_test.cpp
    src/index/content_agent.cpp
    src/index/content_instant_session.cpp
    src/index/content_index.cpp src/index/content_search.cpp src/index/content_search_client.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_content_index_test PRIVATE pulse_sqlite pulse_text bcrypt ole32)
add_dependencies(pulse_content_index_test pulse_index)
add_executable(pulse_search_enhancement_ui_test src/bench/search_enhancement_ui_test.cpp
    src/ui/name_highlight.cpp src/model/item_filter.cpp src/model/search_query.cpp src/index/index_query.cpp
    src/index/search_kinds.cpp src/index/filename_pinyin.cpp)

foreach(paged_target pulse_content_search_test pulse_content_index_test)
    if(TARGET ${paged_target})
        target_sources(${paged_target} PRIVATE src/index/content_result_store.cpp)
        target_link_libraries(${paged_target} PRIVATE pulse_sqlite)
    endif()
endforeach()

add_executable(pulse_content_paging_test src/bench/content_paging_test.cpp
    src/index/content_result_store.cpp src/index/content_index.cpp src/index/content_search.cpp
    src/index/content_search_client.cpp src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_content_paging_test PRIVATE pulse_sqlite pulse_text bcrypt ole32)
add_dependencies(pulse_content_paging_test pulse_index)

add_executable(pulse_content_stream_scaling_test EXCLUDE_FROM_ALL
    src/bench/content_stream_scaling_test.cpp src/index/content_result_store.cpp)
target_link_libraries(pulse_content_stream_scaling_test PRIVATE pulse_sqlite advapi32 ole32 user32)

add_executable(pulse_app_core_remaining_test EXCLUDE_FROM_ALL src/bench/app_core_remaining_test.cpp)
target_link_libraries(pulse_app_core_remaining_test PRIVATE user32 shell32 ole32 advapi32 uuid)

add_executable(pulse_app_integrations_recovery_test EXCLUDE_FROM_ALL
    src/bench/app_integrations_recovery_test.cpp src/app/system_ctx_registry.cpp
    src/app/system_ctx_model.cpp src/app/places.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_compile_options(pulse_app_integrations_recovery_test PRIVATE /Gy /Gw)
target_link_options(pulse_app_integrations_recovery_test PRIVATE /OPT:REF)
target_link_libraries(pulse_app_integrations_recovery_test PRIVATE
    advapi32 shell32 shlwapi ole32 version uuid user32 mpr netapi32)

add_executable(pulse_audit_common_remaining_test EXCLUDE_FROM_ALL
    src/bench/audit_common_remaining_test.cpp src/model/search_query.cpp src/model/batch_rename.cpp
    src/fs/fs_enum.cpp src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_audit_common_remaining_test PRIVATE ole32 shell32 uuid shlwapi netapi32)

add_executable(pulse_jpeg_orientation_test EXCLUDE_FROM_ALL
    src/bench/jpeg_orientation_test.cpp src/preview_host/image_decoder.cpp)
target_link_libraries(pulse_jpeg_orientation_test PRIVATE ole32 windowscodecs uuid)

add_executable(pulse_audit_ui_identity_ime_test EXCLUDE_FROM_ALL src/bench/audit_ui_identity_ime_test.cpp)
target_compile_options(pulse_audit_ui_identity_ime_test PRIVATE /Gy /Gw)
target_link_options(pulse_audit_ui_identity_ime_test PRIVATE /OPT:REF)
target_link_libraries(pulse_audit_ui_identity_ime_test PRIVATE
    user32 gdi32 comctl32 ole32 shell32 uuid windowscodecs)

add_executable(pulse_delete_confirmation_owner_test EXCLUDE_FROM_ALL src/bench/delete_confirmation_owner_test.cpp)
target_link_libraries(pulse_delete_confirmation_owner_test PRIVATE user32)

add_executable(pulse_bitmap_artifact_test EXCLUDE_FROM_ALL src/bench/bitmap_artifact_test.cpp)
target_link_libraries(pulse_bitmap_artifact_test PRIVATE ole32)

add_executable(pulse_audit_ui_remaining_test EXCLUDE_FROM_ALL
    src/bench/audit_ui_remaining_test.cpp src/bench/audit_ui_remaining_test.rc
    src/ui/advanced_search_dialog.cpp src/ui/batch_rename_dialog.cpp
    src/ui/connect_server_dialog.cpp src/ui/file_operation_dialog.cpp src/ui/text_diff_window.cpp
    src/ui/quick_preview_window.cpp src/ui/quick_preview_playback.cpp src/ui/preview_handler_host.cpp
    src/ui/video_preview.cpp src/ui/thumbnail_cache.cpp src/ui/thumbnail_disk_cache.cpp
    src/ui/color_picker.cpp src/ui/fluent_menu.cpp src/ui/fluent_components.cpp src/ui/typography.cpp
    src/ui/ui_compositor.cpp src/ui/window_material.cpp src/ui/window_helpers.cpp
    src/ui/dialog_lifecycle.cpp src/ui/tab_shape.cpp src/ui/edit_host.cpp src/ui/svg_bitmap.cpp
    src/model/batch_rename.cpp src/model/search_query.cpp src/index/index_query.cpp
    src/index/search_kinds.cpp src/index/filename_pinyin.cpp src/fs/fs_enum.cpp
    src/common/localization.cpp src/common/text_decode.cpp src/app/text_diff.cpp
    src/ops/file_lock_owner.cpp src/ops/clipboard.cpp src/ipc/shell_interaction_client.cpp
    src/ipc/shell_query_client.cpp src/ipc/shell_client.cpp)
target_include_directories(pulse_audit_ui_remaining_test PRIVATE src "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(pulse_audit_ui_remaining_test PRIVATE PULSE_UI_TEST_HOOKS=1)
target_compile_options(pulse_audit_ui_remaining_test PRIVATE /Gy /Gw)
target_link_options(pulse_audit_ui_remaining_test PRIVATE /OPT:REF /MANIFEST:NO)
target_link_libraries(pulse_audit_ui_remaining_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 oleaut32 shell32 uuid comctl32
    user32 gdi32 windowscodecs advapi32 shlwapi netapi32 rstrtmgr mfuuid propsys bcrypt)
pulse_enable_lumatext(pulse_audit_ui_remaining_test)

add_executable(pulse_content_search_isolation_test EXCLUDE_FROM_ALL
    src/bench/content_search_isolation_test.cpp src/index/content_search_client.cpp
    src/index/content_search_session.cpp src/index/content_result_store.cpp)
target_link_libraries(pulse_content_search_isolation_test PRIVATE
    pulse_user_storage pulse_sqlite pulse_localization pulse_text
    advapi32 shlwapi ole32 shell32 user32 uuid)
add_dependencies(pulse_content_search_isolation_test pulse_index)

foreach(content_client_target pulse pulse_content_search_test pulse_content_index_test pulse_content_paging_test)
    target_link_libraries(${content_client_target} PRIVATE pulse_user_storage)
    target_sources(${content_client_target} PRIVATE src/index/content_search_session.cpp)
endforeach()

# Document parsers never load into the UI or the persistent index writer.
add_library(pulse_document_reader STATIC src/index/document_reader.cpp)
target_link_libraries(pulse_document_reader PUBLIC pulse_text shell32 ole32)
add_executable(pulse_document src/index/document_host_main.cpp
    src/index/document_filter.cpp src/index/office_ooxml.cpp src/index/pdfium_text.cpp)
add_custom_command(TARGET pulse_document POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll" "$<TARGET_FILE_DIR:pulse_document>/pdfium.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/licenses" "$<TARGET_FILE_DIR:pulse_document>/licenses/PDFium"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/LICENSE" "$<TARGET_FILE_DIR:pulse_document>/licenses/PDFium/binary-distribution.txt")
set_target_properties(pulse_document PROPERTIES OUTPUT_NAME "Pulse.Document")
target_link_libraries(pulse_document PRIVATE ole32 uuid xmllite shlwapi advapi32)
add_executable(pulse_pdf_bench src/bench/pdf_bench.cpp)
add_executable(pulse_document_admission_test src/bench/document_admission_test.cpp)
add_executable(pulse_document_failure_fixture src/bench/document_failure_host.cpp)
add_executable(pulse_document_pool_bench src/bench/document_pool_bench.cpp)
target_link_libraries(pulse_document_pool_bench PRIVATE pulse_document_reader psapi)
add_dependencies(pulse_document_pool_bench pulse_document)
target_link_libraries(pulse_pdf_bench PRIVATE pulse_document_reader ole32 psapi)
add_dependencies(pulse_pdf_bench pulse_document)
pulse_add_version_resource(pulse_document "Pulse document text extractor" "Pulse.Document" "Pulse.Document.exe")
foreach(document_target pulse pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test)
    target_link_libraries(${document_target} PRIVATE pulse_document_reader)
    add_dependencies(${document_target} pulse_document)
endforeach()
add_executable(pulse_office_ooxml_test src/bench/office_ooxml_test.cpp src/index/office_ooxml.cpp)
target_include_directories(pulse_office_ooxml_test PRIVATE src)
target_link_libraries(pulse_office_ooxml_test PRIVATE ole32 uuid xmllite)
add_executable(pulse_office_index_test src/bench/office_index_test.cpp
    src/index/content_index.cpp src/index/content_search.cpp src/index/content_result_store.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_office_index_test PRIVATE pulse_user_storage pulse_document_reader pulse_sqlite ole32 uuid bcrypt psapi)
add_dependencies(pulse_office_index_test pulse_document)
add_library(pulse_content_task STATIC src/index/content_task_search.cpp src/index/text_task_reader.cpp src/index/content_timing.cpp)
target_link_libraries(pulse_content_task PUBLIC pulse_document_reader pulse_text)
add_executable(pulse_content_timing_test src/bench/content_timing_test.cpp src/index/content_timing.cpp)
add_executable(pulse_text_task_test src/bench/text_task_reader_test.cpp src/index/text_task_reader.cpp)
target_link_libraries(pulse_text_task_test PRIVATE pulse_text)
foreach(task_target pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test pulse_office_index_test)
    target_link_libraries(${task_target} PRIVATE pulse_content_task)
endforeach()

add_library(pulse_fs_watch STATIC src/fs/fs_watch.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_fs_watch PUBLIC shell32 shlwapi netapi32)
foreach(watch_target pulse pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test pulse_office_index_test)
    target_link_libraries(${watch_target} PRIVATE pulse_fs_watch)
endforeach()

# Configuration storage is shared without linking the body extraction engine into the UI.
get_property(configuration_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(configuration_target IN LISTS configuration_targets)
    get_target_property(configuration_sources ${configuration_target} SOURCES)
    if("${configuration_sources}" MATCHES "content_(index|search_client)\\.cpp" AND NOT "${configuration_sources}" MATCHES "content_configuration\\.cpp")
        target_sources(${configuration_target} PRIVATE src/index/content_configuration.cpp)
    endif()
endforeach()

# Opt-in application fixtures reuse the production wiring without running the
# legacy selftest suite. They must be requested explicitly during local checks.
option(PULSE_BUILD_ARCHITECTURE_FIXTURES "Build architecture integration fixtures" OFF)
if(PULSE_BUILD_ARCHITECTURE_FIXTURES)
    get_target_property(architecture_sources pulse SOURCES)
    get_target_property(architecture_libraries pulse LINK_LIBRARIES)
    add_executable(pulse_entry_order_application_test
        src/bench/entry_order_application_fixture_test.cpp ${architecture_sources})
    add_executable(pulse_directory_contract_application_test
        src/bench/directory_contract_application_test.cpp ${architecture_sources})
    add_executable(pulse_notify_patch_application_test
        src/bench/notify_patch_application_test.cpp ${architecture_sources})
    add_executable(pulse_filename_search_application_test
        src/bench/filename_search_application_test.cpp ${architecture_sources})
    add_executable(pulse_rename_enter_test
        src/bench/rename_enter_test.cpp ${architecture_sources})
    add_executable(pulse_archive_navigation_test
        src/bench/archive_navigation_test.cpp ${architecture_sources})
    add_executable(pulse_multiwindow_application_test
        src/bench/multiwindow_application_test.cpp ${architecture_sources})
    add_executable(pulse_ops_journal_test
        src/bench/ops_journal_test_main.cpp ${architecture_sources})
    add_executable(pulse_titlebar_layout_test
        src/bench/titlebar_layout_test.cpp ${architecture_sources})
    add_executable(pulse_permission_operation_prompt_ui_test
        src/bench/permission_operation_prompt_ui_test.cpp ${architecture_sources})
    add_executable(pulse_pane_animation_application_test
        src/bench/pane_animation_application_test.cpp src/bench/content_reveal_ui_test.cpp ${architecture_sources})
    add_executable(pulse_file_selection_ui_test
        src/bench/file_selection_ui_test.cpp src/bench/sidebar_starred_ui_test.cpp
        src/bench/window_size_lock_ui_test.cpp src/bench/tab_indicator_ui_test.cpp
        src/bench/status_bar_ui_test.cpp src/bench/archive_presentation_ui_test.cpp
        src/bench/recycle_toolbar_ui_test.cpp
        src/bench/operation_center_application_test.cpp ${architecture_sources})
    add_executable(pulse_group_toolbar_ui_test
        src/bench/group_toolbar_ui_test.cpp ${architecture_sources})
    # These regressions share one production object set and need no private rsp files.
    add_library(pulse_quality_application_objects OBJECT ${architecture_sources})
    target_link_libraries(pulse_quality_application_objects PRIVATE ${architecture_libraries})
    foreach(quality_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
        get_target_property(quality_value pulse ${quality_property})
        if(quality_value)
            set_property(TARGET pulse_quality_application_objects PROPERTY ${quality_property} "${quality_value}")
        endif()
    endforeach()
    add_executable(pulse_hosted_edit_stability_test src/bench/hosted_edit_stability_test.cpp)
    add_executable(pulse_address_edit_stability_test src/bench/address_edit_stability_test.cpp)
    add_executable(pulse_removable_search_routing_test src/bench/removable_search_routing_test.cpp)
    add_executable(pulse_folder_sizes_ui_cache_test src/bench/folder_sizes_ui_cache_test.cpp)
    add_executable(pulse_text_render_settings_test src/bench/text_render_settings_test.cpp)
    add_executable(pulse_context_menu_settings_ui_test src/bench/context_menu_settings_ui_test.cpp)
    add_executable(pulse_empty_state_render_test src/bench/empty_state_render_test.cpp)
    add_executable(pulse_virtual_location_navigation_test src/bench/virtual_location_navigation_test.cpp)
    add_executable(pulse_upstream_adaptation_test src/bench/upstream_adaptation_test.cpp
        src/bench/window_transfer_regression_test.cpp src/bench/network_access_application_test.cpp)
    target_compile_definitions(pulse_quality_application_objects PRIVATE PULSE_FOLDER_SIZES_TESTING=1)
    target_compile_definitions(pulse_folder_sizes_ui_cache_test PRIVATE PULSE_FOLDER_SIZES_TESTING=1)
    foreach(quality_target pulse_hosted_edit_stability_test pulse_address_edit_stability_test pulse_removable_search_routing_test pulse_folder_sizes_ui_cache_test pulse_text_render_settings_test pulse_upstream_adaptation_test pulse_context_menu_settings_ui_test pulse_empty_state_render_test pulse_virtual_location_navigation_test)
        target_link_libraries(${quality_target} PRIVATE pulse_quality_application_objects ${architecture_libraries})
        target_include_directories(${quality_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        foreach(quality_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
            get_target_property(quality_value pulse ${quality_property})
            if(quality_value)
                set_property(TARGET ${quality_target} APPEND PROPERTY ${quality_property} "${quality_value}")
            endif()
        endforeach()
        if(NOT quality_target MATCHES "^pulse_(context_menu_settings_ui|empty_state_render|virtual_location_navigation)_test$")
            add_dependencies(${quality_target} pulse)
        endif()
    endforeach()
    set(global_search_fixture_sources ${architecture_sources})
    list(FILTER global_search_fixture_sources EXCLUDE REGEX "global_search_window\\.cpp$")
    add_executable(pulse_global_search_window_test
        src/bench/global_search_window_test.cpp ${global_search_fixture_sources})
    add_executable(pulse_playback_ui_test
        src/bench/playback_ui_test.cpp src/bench/localization_test.rc
        src/ui/quick_preview_window.cpp src/ui/quick_preview_playback.cpp
        src/ui/thumbnail_cache.cpp src/ui/thumbnail_disk_cache.cpp
        src/ui/preview_handler_host.cpp src/ui/video_preview.cpp src/ui/window_material.cpp
        src/ui/fluent_menu.cpp src/ui/edit_host.cpp src/ui/ui_compositor.cpp
        src/ui/fluent_components.cpp src/ui/typography.cpp src/ui/tab_shape.cpp
        src/ui/dialog_lifecycle.cpp src/ops/clipboard.cpp)
    target_link_libraries(pulse_playback_ui_test PRIVATE ${architecture_libraries})
    pulse_enable_lumatext(pulse_playback_ui_test)
    add_dependencies(pulse_playback_ui_test pulse_preview)
    foreach(architecture_target pulse_entry_order_application_test pulse_directory_contract_application_test pulse_notify_patch_application_test pulse_global_search_window_test pulse_filename_search_application_test pulse_rename_enter_test pulse_archive_navigation_test pulse_multiwindow_application_test pulse_ops_journal_test pulse_titlebar_layout_test pulse_permission_operation_prompt_ui_test pulse_pane_animation_application_test pulse_file_selection_ui_test pulse_group_toolbar_ui_test)
        target_link_libraries(${architecture_target} PRIVATE ${architecture_libraries})
        foreach(architecture_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
            get_target_property(architecture_value pulse ${architecture_property})
            if(architecture_value)
                set_property(TARGET ${architecture_target} PROPERTY ${architecture_property} "${architecture_value}")
            endif()
        endforeach()
    endforeach()
    target_compile_definitions(pulse_permission_operation_prompt_ui_test PRIVATE PULSE_UI_TEST_HOOKS=1)
    target_compile_definitions(pulse_directory_contract_application_test PRIVATE PULSE_WORKER_TESTING=1 PULSE_NET_CACHE_TESTING=1)
endif()

# Opt-in integration fixture for content-search progress rendering.
option(PULSE_BUILD_PROGRESS_UI_TESTS "Build the content-search progress renderer fixture" OFF)
if(PULSE_BUILD_PROGRESS_UI_TESTS)
    get_target_property(progress_ui_sources pulse SOURCES)
    get_target_property(progress_ui_libraries pulse LINK_LIBRARIES)
    add_executable(pulse_content_progress_ui_test src/bench/content_progress_ui_test.cpp ${progress_ui_sources})
    target_link_libraries(pulse_content_progress_ui_test PRIVATE ${progress_ui_libraries})
    foreach(progress_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
        get_target_property(progress_value pulse ${progress_property})
        if(progress_value)
            set_property(TARGET pulse_content_progress_ui_test PROPERTY ${progress_property} "${progress_value}")
        endif()
    endforeach()
endif()

add_executable(pulse_transfer_rate_test src/bench/transfer_rate_test.cpp)
target_include_directories(pulse_transfer_rate_test PRIVATE src)

add_executable(pulse_copy_progress_test src/bench/copy_progress_test.cpp)
target_include_directories(pulse_copy_progress_test PRIVATE src)

add_executable(pulse_archive_test src/bench/archive_test.cpp src/ops/archive.cpp)
target_include_directories(pulse_archive_test PRIVATE src)
target_compile_definitions(pulse_archive_test PRIVATE PULSE_ARCHIVE_PROCESS_TEST)
add_executable(pulse_archive_process_fixture src/bench/archive_process_fixture.cpp)
add_dependencies(pulse_archive_test pulse_archive_process_fixture)
add_executable(pulse_net_cache_test src/bench/net_cache_test.cpp
    src/fs/fs_net_cache.cpp src/fs/fs_enum.cpp)
target_include_directories(pulse_net_cache_test PRIVATE src)
target_compile_definitions(pulse_net_cache_test PRIVATE PULSE_NET_CACHE_TESTING)
target_link_libraries(pulse_net_cache_test PRIVATE shell32 shlwapi netapi32)
add_executable(pulse_archive_dialog_test src/bench/archive_dialog_test.cpp
    src/app/archive_dialog.cpp src/app/archive_operation_ui.cpp src/bench/localization_test.rc
    src/ui/file_operation_dialog.cpp src/ui/fluent_components.cpp src/ui/window_helpers.cpp
    src/ui/tab_shape.cpp src/ui/dialog_lifecycle.cpp src/ui/window_material.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ops/file_lock_owner.cpp)
target_include_directories(pulse_archive_dialog_test PRIVATE src)
target_link_libraries(pulse_archive_dialog_test PRIVATE pulse_localization
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid
    comctl32 user32 gdi32 windowscodecs advapi32 rstrtmgr)
pulse_enable_lumatext(pulse_archive_dialog_test)
add_custom_command(TARGET pulse_archive_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/7zip" "$<TARGET_FILE_DIR:pulse_archive_test>/7zip")

get_property(shell_interaction_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(shell_interaction_target IN LISTS shell_interaction_targets)
    get_target_property(shell_interaction_sources ${shell_interaction_target} SOURCES)
    if(NOT shell_interaction_target STREQUAL "pulse" AND
       "${shell_interaction_sources}" MATCHES "(index_client|advanced_search_dialog|archive_dialog)\\.cpp")
        target_link_libraries(${shell_interaction_target} PRIVATE pulse_shell_interaction_client)
    endif()
    if(NOT shell_interaction_target STREQUAL "pulse" AND
       "${shell_interaction_sources}" MATCHES "preview_handler_host\\.cpp")
        target_link_libraries(${shell_interaction_target} PRIVATE pulse_shell_query_client)
    endif()
endforeach()

add_executable(pulse_recent_day_label_test src/bench/recent_day_label_test.cpp)

add_executable(pulse_drop_shortcuts_test src/bench/drop_shortcuts_test.cpp
    src/app/app_drop_shortcuts_io.cpp)
target_link_libraries(pulse_drop_shortcuts_test PRIVATE ole32 shell32 uuid)

add_executable(pulse_device_source_test src/bench/device_source_test.cpp
    src/fs/device_path.cpp src/fs/device_fake.cpp src/fs/device_shell.cpp src/fs/device_worker.cpp)
target_link_libraries(pulse_device_source_test PRIVATE shell32 ole32 uuid propsys cfgmgr32 shlwapi advapi32)

add_executable(pulse_settings_transfer_test src/bench/settings_transfer_test.cpp
    src/app/settings_transfer.cpp)

add_executable(pulse_network_locations_feature_test src/bench/network_locations_feature_test.cpp
    src/app/server_address.cpp src/app/network_recent.cpp src/app/network_recent_store.cpp
    src/app/network_locations.cpp src/app/system_network_locations.cpp
    src/app/lan_device.cpp src/app/lan_discovery.cpp)
target_link_libraries(pulse_network_locations_feature_test PRIVATE pulse_user_storage
    pulse_localization shell32 ole32 oleaut32 uuid propsys shlwapi advapi32)

get_target_property(network_reload_sources pulse_index_test SOURCES)
list(FILTER network_reload_sources EXCLUDE REGEX "/bench/")
add_executable(pulse_network_config_reload_test src/bench/network_config_reload_test.cpp ${network_reload_sources})
target_compile_definitions(pulse_network_config_reload_test PRIVATE PULSE_NETWORK_INDEX_TESTING=1)
target_link_libraries(pulse_network_config_reload_test PRIVATE pulse_user_storage shell32 mpr advapi32)

add_executable(pulse_cloud_features_test src/bench/cloud_features_test.cpp
    src/fs/cloud_hydrate.cpp src/fs/cloud_pin.cpp src/app/cloud_roots.cpp src/app/cloud_root_catalog.cpp src/app/cloud_sync_probe.cpp
    src/app/cloud_download.cpp)
target_link_libraries(pulse_cloud_features_test PRIVATE pulse_localization advapi32 shell32 shlwapi ole32 uuid propsys)

get_target_property(feature_sync_sources pulse SOURCES)
add_executable(pulse_feature_sync_application_test EXCLUDE_FROM_ALL
    src/bench/feature_sync_application_test.cpp ${feature_sync_sources})
get_target_property(feature_sync_libraries pulse LINK_LIBRARIES)

add_executable(pulse_item_reflow_animation_test EXCLUDE_FROM_ALL
    src/bench/item_reflow_animation_test.cpp)
target_compile_options(pulse_item_reflow_animation_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_item_reflow_geometry_test EXCLUDE_FROM_ALL src/bench/item_reflow_geometry_test.cpp)
target_compile_options(pulse_item_reflow_geometry_test PRIVATE /W4 /permissive- /utf-8)
add_executable(pulse_shell_query_connection_test EXCLUDE_FROM_ALL src/bench/shell_query_connection_test.cpp)
target_compile_options(pulse_shell_query_connection_test PRIVATE /W4 /permissive- /utf-8)
add_executable(pulse_item_reflow_rows_test EXCLUDE_FROM_ALL src/bench/item_reflow_rows_test.cpp)
target_compile_options(pulse_item_reflow_rows_test PRIVATE /W4 /permissive- /utf-8)
add_executable(pulse_item_reflow_bitmap_test EXCLUDE_FROM_ALL
    src/bench/item_reflow_bitmap_test.cpp src/ui/animation_image_cache.cpp)
target_compile_options(pulse_item_reflow_bitmap_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_view_mode_scroll_anchor_test EXCLUDE_FROM_ALL
    src/bench/view_mode_scroll_anchor_test.cpp src/ui/view_layout.cpp)
target_compile_options(pulse_view_mode_scroll_anchor_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_item_reflow_ui_test EXCLUDE_FROM_ALL
    src/bench/item_reflow_ui_test.cpp ${feature_sync_sources})
target_link_libraries(pulse_item_reflow_ui_test PRIVATE ${feature_sync_libraries})
foreach(reflow_fixture_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(reflow_fixture_value pulse ${reflow_fixture_property})
    if(reflow_fixture_value)
        set_property(TARGET pulse_item_reflow_ui_test PROPERTY ${reflow_fixture_property} "${reflow_fixture_value}")
    endif()
endforeach()

target_link_libraries(pulse_feature_sync_application_test PRIVATE ${feature_sync_libraries})
foreach(feature_sync_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(feature_sync_value pulse ${feature_sync_property})
    if(feature_sync_value)
        set_property(TARGET pulse_feature_sync_application_test PROPERTY ${feature_sync_property} "${feature_sync_value}")
    endif()
endforeach()

# The text renderer regression can be built without enabling all architecture fixtures.
if(NOT TARGET pulse_text_render_settings_test)
    add_executable(pulse_text_render_settings_test EXCLUDE_FROM_ALL
        src/bench/text_render_settings_test.cpp ${feature_sync_sources})
    target_link_libraries(pulse_text_render_settings_test PRIVATE ${feature_sync_libraries})
    foreach(text_fixture_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
        get_target_property(text_fixture_value pulse ${text_fixture_property})
        if(text_fixture_value)
            set_property(TARGET pulse_text_render_settings_test PROPERTY ${text_fixture_property} "${text_fixture_value}")
        endif()
    endforeach()
endif()

# Exercise startup and wakeups through the production window procedure.
get_target_property(startup_idle_sources pulse SOURCES)
get_target_property(startup_idle_libraries pulse LINK_LIBRARIES)
add_executable(pulse_startup_idle_application_test EXCLUDE_FROM_ALL
    src/bench/startup_idle_application_test.cpp ${startup_idle_sources})
target_link_libraries(pulse_startup_idle_application_test PRIVATE ${startup_idle_libraries})
foreach(startup_idle_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(startup_idle_value pulse ${startup_idle_property})
    if(startup_idle_value)
        set_property(TARGET pulse_startup_idle_application_test PROPERTY ${startup_idle_property} "${startup_idle_value}")
    endif()
endforeach()

add_executable(pulse_startup_idle_test EXCLUDE_FROM_ALL src/bench/startup_idle_test.cpp)
target_link_libraries(pulse_startup_idle_test PRIVATE user32)

add_executable(pulse_thumbnail_pipeline_test EXCLUDE_FROM_ALL
    src/bench/thumbnail_pipeline_test.cpp src/ui/thumbnail_cache.cpp src/ui/thumbnail_disk_cache.cpp)
target_link_libraries(pulse_thumbnail_pipeline_test PRIVATE ole32 shell32 windowscodecs uuid)
add_dependencies(pulse_thumbnail_pipeline_test pulse_preview)

get_target_property(transfer_pipeline_sources pulse_locked_operation_test SOURCES)
list(REMOVE_ITEM transfer_pipeline_sources src/bench/locked_operation_test.cpp)
get_target_property(transfer_pipeline_libraries pulse_locked_operation_test LINK_LIBRARIES)
add_executable(pulse_transfer_pipeline_test EXCLUDE_FROM_ALL
    src/bench/transfer_pipeline_test.cpp ${transfer_pipeline_sources})
target_include_directories(pulse_transfer_pipeline_test PRIVATE src)
target_link_libraries(pulse_transfer_pipeline_test PRIVATE ${transfer_pipeline_libraries})

add_executable(pulse_folder_actions_test EXCLUDE_FROM_ALL
    src/bench/folder_actions_test.cpp src/bench/localization_test.rc ${transfer_pipeline_sources})
target_include_directories(pulse_folder_actions_test PRIVATE src)
target_link_libraries(pulse_folder_actions_test PRIVATE ${transfer_pipeline_libraries})

get_target_property(folder_actions_menu_sources pulse_app_controllers_test SOURCES)
list(FILTER folder_actions_menu_sources EXCLUDE REGEX "app_controllers_test\\.cpp$")
get_target_property(folder_actions_menu_libraries pulse_app_controllers_test LINK_LIBRARIES)
add_executable(pulse_folder_actions_menu_test EXCLUDE_FROM_ALL
    src/bench/folder_actions_menu_test.cpp ${folder_actions_menu_sources})
target_link_libraries(pulse_folder_actions_menu_test PRIVATE ${folder_actions_menu_libraries})

get_property(folder_operation_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(folder_operation_target IN LISTS folder_operation_targets)
    get_target_property(folder_operation_sources ${folder_operation_target} SOURCES)
    if("${folder_operation_sources}" MATCHES "ops_manager\\.cpp" AND
       NOT "${folder_operation_sources}" MATCHES "folder_operations\\.cpp")
        target_sources(${folder_operation_target} PRIVATE src/ops/folder_operations.cpp)
    endif()
endforeach()
