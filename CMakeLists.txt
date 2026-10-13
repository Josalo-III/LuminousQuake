cmake_minimum_required(VERSION 3.20)
project(LuminousQuake VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(LQ_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(CTest)

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
    WinQuake/vid_null.c WinQuake/in_null.c WinQuake/snd_null.c WinQuake/cd_null.c
    WinQuake/net_bsd.c platform/net_udp.c platform/diagnostics.c
)
target_include_directories(quake_core PUBLIC WinQuake platform)
target_compile_definitions(quake_core PUBLIC LQ_PORTABLE=1)
if(WIN32)
    target_compile_definitions(quake_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
endif()

add_executable(luminousquake platform/sys_portable.c)
add_executable(luminousquake-server platform/sys_portable.c)
target_compile_definitions(luminousquake-server PRIVATE LQ_DEDICATED=1)
foreach(target quake_core luminousquake luminousquake-server)
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
foreach(target luminousquake luminousquake-server)
    target_link_libraries(${target} PRIVATE quake_core)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    else()
        target_link_libraries(${target} PRIVATE m)
    endif()
endforeach()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME udp-loopback COMMAND luminousquake --self-test --test-udp -ip 127.0.0.1)
    foreach(target luminousquake luminousquake-server)
        add_test(NAME ${target}-version COMMAND ${target} --version)
        add_test(NAME ${target}-core COMMAND ${target} --self-test)
        add_test(NAME ${target}-fixtures COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:${target}>)
    endforeach()
endif()
