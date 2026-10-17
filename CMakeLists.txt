cmake_minimum_required(VERSION 3.20)
project(LuminousQuake VERSION 0.3.0 LANGUAGES C)
find_package(ZLIB REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(LQ_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LQ_BUILD_CLIENT "Build the SDL windowed client" ON)
option(LQ_BUILD_GPU "Build the experimental SDL3 GPU world viewer" OFF)
option(LQ_GPU_TESTS "Run tests requiring an actual SDL3 GPU device/window system" OFF)
include(CTest)
add_library(lq_scene STATIC renderer/scene.c renderer/scene_fixture.c renderer/indexed_mips.c renderer/bounce.c renderer/effects.c
    renderer/fluorescent.c)
target_include_directories(lq_scene PUBLIC renderer)
if(NOT WIN32)
    target_link_libraries(lq_scene PUBLIC m)
endif()
if(BUILD_TESTING)
    add_executable(lq-scene-test tests/renderer_scene.c)
    target_link_libraries(lq-scene-test PRIVATE lq_scene)
    add_test(NAME renderer-scene-contract COMMAND lq-scene-test)
endif()

add_library(quake_core OBJECT
    WinQuake/cl_demo.c
    WinQuake/cl_input.c
    WinQuake/cl_main.c
    WinQuake/cl_parse.c
    WinQuake/cl_tent.c
    WinQuake/chase.c
    WinQuake/cmd.c
    WinQuake/common.c
    WinQuake/console.c
    WinQuake/crc.c
    WinQuake/cvar.c
    WinQuake/draw.c
    WinQuake/d_edge.c
    WinQuake/d_fill.c
    WinQuake/d_init.c
    WinQuake/d_modech.c
    WinQuake/d_part.c
    WinQuake/d_polyse.c
    WinQuake/d_scan.c
    WinQuake/d_sky.c
    WinQuake/d_sprite.c
    WinQuake/d_surf.c
    WinQuake/d_vars.c
    WinQuake/d_zpoint.c
    WinQuake/host.c
    WinQuake/host_cmd.c
    WinQuake/keys.c
    WinQuake/menu.c
    WinQuake/mathlib.c
    WinQuake/model.c
    WinQuake/net_dgrm.c
    WinQuake/net_loop.c
    WinQuake/net_main.c
    WinQuake/net_vcr.c
    WinQuake/nonintel.c
    WinQuake/pr_cmds.c
    WinQuake/pr_edict.c
    WinQuake/pr_exec.c
    WinQuake/r_aclip.c
    WinQuake/r_alias.c
    WinQuake/r_bsp.c
    WinQuake/r_light.c
    WinQuake/r_draw.c
    WinQuake/r_efrag.c
    WinQuake/r_edge.c
    WinQuake/r_misc.c
    WinQuake/r_main.c
    WinQuake/r_sky.c
    WinQuake/r_sprite.c
    WinQuake/r_surf.c
    WinQuake/r_part.c
    WinQuake/r_vars.c
    WinQuake/screen.c
    WinQuake/sbar.c
    WinQuake/sv_main.c
    WinQuake/sv_phys.c
    WinQuake/sv_move.c
    WinQuake/sv_user.c
    WinQuake/zone.c
    WinQuake/view.c
    WinQuake/wad.c
    WinQuake/world.c
    WinQuake/cd_null.c
    WinQuake/net_bsd.c platform/net_udp.c platform/diagnostics.c platform/user_paths.c platform/localization.c platform/frame_timing.c
)
target_include_directories(quake_core PUBLIC WinQuake platform)
target_link_libraries(quake_core PUBLIC ZLIB::ZLIB)
target_compile_definitions(quake_core PUBLIC LQ_PORTABLE=1)
if(WIN32)
    target_compile_definitions(quake_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
endif()

set(LQ_HEADLESS_BACKENDS WinQuake/vid_null.c WinQuake/in_null.c WinQuake/snd_null.c platform/render_bridge_null.c)
add_executable(luminousquake-headless platform/sys_portable.c ${LQ_HEADLESS_BACKENDS})
add_executable(luminousquake-server platform/sys_portable.c ${LQ_HEADLESS_BACKENDS})
target_compile_definitions(luminousquake-server PRIVATE LQ_DEDICATED=1)
set(LQ_TARGETS quake_core luminousquake-headless luminousquake-server lq_scene)
if(BUILD_TESTING)
    list(APPEND LQ_TARGETS lq-scene-test)
endif()
set(LQ_EXECUTABLES luminousquake-headless luminousquake-server)
if(LQ_BUILD_CLIENT)
    find_package(SDL3 3.4 CONFIG REQUIRED)
    add_executable(luminousquake platform/sys_portable.c
        platform/vid_sdl.c platform/in_sdl.c platform/snd_sdl.c platform/demo_sdl.c
        platform/render_bridge.c platform/ui_scale.c platform/jobs_sdl.c platform/gpu_entities.c renderer/quake_scene.c renderer/gpu_sdl.c
        WinQuake/snd_dma.c WinQuake/snd_mem.c WinQuake/snd_mix.c)
    target_compile_definitions(luminousquake PRIVATE LQ_SDL=1 SDL_MAIN_HANDLED)
    target_link_libraries(luminousquake PRIVATE SDL3::SDL3 lq_scene)
    if(APPLE)
        enable_language(OBJC)
        target_sources(luminousquake PRIVATE platform/mac_launch.m)
        target_link_libraries(luminousquake PRIVATE "-framework Cocoa")
        set_property(TARGET luminousquake APPEND PROPERTY BUILD_RPATH "@loader_path")
        get_target_property(LQ_CLIENT_SOURCES luminousquake SOURCES)
        add_executable(luminousquake-app MACOSX_BUNDLE ${LQ_CLIENT_SOURCES})
        target_compile_definitions(luminousquake-app PRIVATE LQ_SDL=1 SDL_MAIN_HANDLED)
        target_link_libraries(luminousquake-app PRIVATE SDL3::SDL3 lq_scene "-framework Cocoa")
        set_target_properties(luminousquake-app PROPERTIES OUTPUT_NAME LuminousQuake
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/mac/Info.plist.in"
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
        set(LQ_ICON "${CMAKE_CURRENT_BINARY_DIR}/LuminousQuake.icns")
        add_custom_command(OUTPUT "${LQ_ICON}"
            COMMAND /usr/bin/swift -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/SwiftModuleCache" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mac_icon.swift" "${CMAKE_CURRENT_BINARY_DIR}/LuminousQuake.iconset"
            COMMAND /usr/bin/iconutil -c icns "${CMAKE_CURRENT_BINARY_DIR}/LuminousQuake.iconset" -o "${LQ_ICON}"
            DEPENDS tools/mac_icon.swift VERBATIM)
        set_source_files_properties("${LQ_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(luminousquake-app PRIVATE "${LQ_ICON}")
        # Relink on a new icon so the post-build step signs the changed bundle.
        set_property(TARGET luminousquake-app APPEND PROPERTY LINK_DEPENDS "${LQ_ICON}")
        # Packaging changes must refresh existing apps even when no C source changed.
        set_property(TARGET luminousquake-app APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_mac.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_mac_bundle.cmake")
        add_custom_command(TARGET luminousquake-app POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:luminousquake-app>"
                "-DEXECUTABLE=$<TARGET_FILE:luminousquake-app>" "-DSDL_LIBRARY=$<TARGET_FILE:SDL3::SDL3>"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_mac.cmake"
            VERBATIM)
        list(APPEND LQ_TARGETS luminousquake-app)
        list(APPEND LQ_EXECUTABLES luminousquake-app)
    endif()
    if(WIN32 OR APPLE)
        add_custom_command(TARGET luminousquake POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:luminousquake>)
    endif()
    list(APPEND LQ_TARGETS luminousquake)
    list(APPEND LQ_EXECUTABLES luminousquake)
endif()
if(LQ_BUILD_GPU)
    find_package(SDL3 3.4 CONFIG REQUIRED)
    add_executable(luminousquake-gpu platform/sys_portable.c platform/gpu_viewer.c platform/jobs_sdl.c
        renderer/quake_scene.c renderer/gpu_sdl.c ${LQ_HEADLESS_BACKENDS})
    target_compile_definitions(luminousquake-gpu PRIVATE LQ_GPU=1 SDL_MAIN_HANDLED)
    target_link_libraries(luminousquake-gpu PRIVATE lq_scene SDL3::SDL3)
    if(APPLE)
        set_property(TARGET luminousquake-gpu APPEND PROPERTY BUILD_RPATH "@loader_path")
    endif()
    get_target_property(LQ_SDL3_INCLUDES SDL3::Headers INTERFACE_INCLUDE_DIRECTORIES)
    find_file(LQ_SDL3_LICENSE LICENSE.txt PATHS
        "${LQ_SDL3_INCLUDES}/.." "${LQ_SDL3_INCLUDES}/../share/licenses/SDL3" NO_DEFAULT_PATH)
    if(LQ_SDL3_LICENSE)
        add_custom_command(TARGET luminousquake-gpu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LQ_SDL3_LICENSE}" "$<TARGET_FILE_DIR:luminousquake-gpu>/SDL3-LICENSE.txt")
    endif()
    if(WIN32 OR APPLE)
        add_custom_command(TARGET luminousquake-gpu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:luminousquake-gpu>)
    endif()
    if(BUILD_TESTING AND LQ_GPU_TESTS)
        add_test(NAME gpu-render-readback COMMAND luminousquake-gpu --gpu-test)
        set_tests_properties(gpu-render-readback PROPERTIES TIMEOUT 30)
    endif()
    list(APPEND LQ_TARGETS luminousquake-gpu)
    list(APPEND LQ_EXECUTABLES luminousquake-gpu)
endif()
foreach(target IN LISTS LQ_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter
            -Wno-missing-field-initializers -fno-strict-aliasing -ffp-contract=off)
    endif()
    if(LQ_SANITIZERS)
        if(MSVC)
            message(FATAL_ERROR "LQ_SANITIZERS currently requires Clang or GCC")
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
foreach(target IN LISTS LQ_EXECUTABLES)
    target_link_libraries(${target} PRIVATE quake_core)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    else()
        target_link_libraries(${target} PRIVATE m)
    endif()
endforeach()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME bsp-light-visibility COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bsp_light_visibility.py)
    set_tests_properties(bsp-light-visibility PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    add_test(NAME udp-loopback COMMAND luminousquake-headless --self-test --test-udp -ip 127.0.0.1)
    foreach(target IN LISTS LQ_EXECUTABLES)
        add_test(NAME ${target}-version COMMAND ${target} --version)
        add_test(NAME ${target}-core COMMAND ${target} --self-test)
        if(NOT target STREQUAL "luminousquake-gpu")
            add_test(NAME ${target}-fixtures COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:${target}>)
        else()
            add_test(NAME gpu-scene-import COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_scene_import.py $<TARGET_FILE:${target}>)
        endif()
    endforeach()
    if(LQ_BUILD_CLIENT)
        if(APPLE)
            add_test(NAME mac-bundle-relocation COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/mac_bundle.py $<TARGET_BUNDLE_DIR:luminousquake-app>)
        endif()
        add_test(NAME sdl-platform COMMAND luminousquake --platform-test -userdir "${CMAKE_CURRENT_BINARY_DIR}/platform-test-user")
        set_tests_properties(sdl-platform PROPERTIES
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy" TIMEOUT 15)
        add_test(NAME sdl-unavailable-audio COMMAND luminousquake --platform-test --expect-no-audio -userdir "${CMAKE_CURRENT_BINARY_DIR}/platform-test-user")
        set_tests_properties(sdl-unavailable-audio PROPERTIES
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=nonexistent" TIMEOUT 15)
    endif()
endif()
