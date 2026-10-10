cmake_minimum_required(VERSION 3.25)
project(rusty VERSION 0.1.0 LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()

# Rusty (PC-98) recompiled to C. The Python tools in tools/recomp generate one C file per game module from the
# unpacked game files; the runtime (runtime/) emulates the PC-98 around them.
#   cmake --preset linux-dev && cmake --build --preset linux-dev
#       -> build/linux-dev/rusty (window) and build/linux-dev/rusty_headless
#   cmake --preset linux-release && cmake --build --preset linux-release && cpack --preset linux-release
#       -> build/packages/rusty-<version>-linux-x64.tar.gz
#   cmake --preset win-release && cmake --build --preset win-release && cpack --preset win-release
#       -> build/packages/rusty-<version>-win-x64.zip (cross build with MinGW-w64)
# Generation needs the unpacked game files (tools/extract.py, build/unpacked) and the project venv (uv sync).

option(RUSTY_RELEASE "Release build: user data folder or portable mode, no missing log, no dev ROM folder" OFF)
option(RUSTY_ENGLISH "Also build the modules of the English translation (46 OkuMen v1.0.3)" ON)

set(RUSTY_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python of the project venv (uv sync)")
set(RUSTY_UNPACKED_JA "${CMAKE_SOURCE_DIR}/build/unpacked/ja" CACHE PATH "Unpacked Japanese game files")
set(RUSTY_UNPACKED_EN "${CMAKE_SOURCE_DIR}/build/unpacked/base" CACHE PATH "Unpacked English game files")
set(RUSTY_GAME_JA "${CMAKE_SOURCE_DIR}/build/files/ja/RUSTY" CACHE PATH "Original Japanese game files (file hashes)")
set(RUSTY_GAME_EN "${CMAKE_SOURCE_DIR}/build/files/base" CACHE PATH "Original English game files (file hashes)")
set(RUSTY_COVERAGE "${CMAKE_SOURCE_DIR}/tools/recomp/coverage" CACHE PATH "Code offsets the original game executed")

set(RECOMP "${CMAKE_SOURCE_DIR}/tools/recomp")
set(NAMES "${CMAKE_SOURCE_DIR}/tools/recomp/names")
set(ENTRIES "${RECOMP}/entries")
set(GEN "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")
file(GLOB GENERATOR_SCRIPTS CONFIGURE_DEPENDS "${RECOMP}/*.py")
# A new names, entries or coverage file changes the generator options: configure again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${NAMES}" "${NAMES}/en" "${ENTRIES}" "${ENTRIES}/en" "${RUSTY_COVERAGE}")

set(RUSTY_MODULES "")
set(RUSTY_SOURCES "")

# rusty_module(<prefix> <ja|en> <file> [NAMES] [COVERAGE] [COVERAGE_TABLES] [ENTRIES] [HANDLERS] [ARGS ...]):
# generate gen/<prefix>_gen.c with recomp.py. NAMES/ENTRIES add the files of tools/recomp/names and
# tools/recomp/entries that exist (for en: the Japanese one, then the English one), COVERAGE the coverage file,
# COVERAGE_TABLES the coverage file with --data-tables, HANDLERS the ENEMYn AI handler table (enemy_handlers.py).
function(rusty_module prefix version file)
    cmake_parse_arguments(PARSE_ARGV 3 M "NAMES;COVERAGE;COVERAGE_TABLES;ENTRIES;HANDLERS" "" "ARGS")
    if(version STREQUAL "en")
        set(input "${RUSTY_UNPACKED_EN}/${file}")
        set(module "en_${prefix}")
        set(name_files "${NAMES}/${file}.tsv" "${NAMES}/en/${file}.tsv")
        set(entry_files "${ENTRIES}/${file}.txt" "${ENTRIES}/en/${file}.txt")
        set(identity --prefix "${module}" --module-name "en/${file}")
    else()
        set(input "${RUSTY_UNPACKED_JA}/${file}")
        set(module "${prefix}")
        set(name_files "${NAMES}/${file}.tsv")
        set(entry_files "${ENTRIES}/${file}.txt")
        set(identity --prefix "${module}")
    endif()
    set(options ${M_ARGS})
    set(depends "${input}" ${GENERATOR_SCRIPTS})
    if(M_NAMES)
        foreach(path IN LISTS name_files)
            if(EXISTS "${path}")
                list(APPEND options --names "${path}")
                list(APPEND depends "${path}")
            endif()
        endforeach()
    endif()
    set(coverage "${RUSTY_COVERAGE}/${file}.txt")
    if((M_COVERAGE OR M_COVERAGE_TABLES) AND EXISTS "${coverage}")
        list(APPEND options --coverage "${coverage}")
        list(APPEND depends "${coverage}")
        if(M_COVERAGE_TABLES)
            list(APPEND options --data-tables)
        endif()
    endif()
    if(M_ENTRIES)
        foreach(path IN LISTS entry_files)
            if(EXISTS "${path}")
                list(APPEND options --entries-file "${path}")
                list(APPEND depends "${path}")
            endif()
        endforeach()
    endif()
    set(output "${GEN}/${module}_gen.c")
    set(commands "")
    if(M_HANDLERS)
        set(handlers "${GEN}/${module}.handlers")
        set(commands COMMAND "${RUSTY_PYTHON}" "${RECOMP}/enemy_handlers.py" "${input}" --out "${handlers}")
        list(APPEND options --entries-file "${handlers}")
    endif()
    add_custom_command(
        OUTPUT "${output}"
        ${commands}
        COMMAND "${RUSTY_PYTHON}" "${RECOMP}/recomp.py" "${input}" ${options} ${identity} --out "${output}"
        DEPENDS ${depends}
        COMMENT "Recompiling ${version}/${file}"
        VERBATIM)
    set(RUSTY_MODULES ${RUSTY_MODULES} "${module}" PARENT_SCOPE)
    set(RUSTY_SOURCES ${RUSTY_SOURCES} "${output}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${RUSTY_PYTHON}")
    message(FATAL_ERROR "${RUSTY_PYTHON} not found: run 'uv sync' in the project root")
endif()

# The module table. The order is the search order of the runtime (rt_modules).
rusty_module(boot ja BOOT.COM NAMES ARGS --entry 100 --entry 316 --entry 278)
rusty_module(ongchk ja ONGCHK.COM NAMES ARGS --entry 100)
rusty_module(clablogo ja CLABLOGO.EXE NAMES COVERAGE ARGS --exe)
rusty_module(op ja OP.COM NAMES ARGS --entry 100)
rusty_module(mgxload ja MGXLOAD.BIN NAMES ARGS --base 0 --entry 0)
foreach(driver IN ITEMS pdr ppsdrv pmdb2)
    string(TOUPPER "${driver}.COM" file)
    rusty_module(${driver} ja ${file} NAMES COVERAGE_TABLES ENTRIES ARGS --entry 100)
endforeach()
set(OVERLAY NAMES COVERAGE ENTRIES ARGS --base 0 --data-tables)
rusty_module(enemy10 ja ENEMY10.COM HANDLERS ${OVERLAY})
foreach(n RANGE 1 9)
    rusty_module(enemy${n} ja ENEMY${n}.COM HANDLERS ${OVERLAY})
endforeach()
rusty_module(grpegc ja GRPEGC.COM ${OVERLAY})
rusty_module(grpgdc ja GRPGDC.COM ${OVERLAY})
# 00D4: the top of the main loop, the snapshot save point (runtime/state.c).
rusty_module(jo ja JO.EXE NAMES COVERAGE ENTRIES ARGS --exe --safe-point 00d4)
rusty_module(magload ja MAGLOAD.COM ${OVERLAY} --entry 0)
rusty_module(story10 ja STORY10.COM ${OVERLAY})
foreach(n RANGE 1 9)
    rusty_module(story${n} ja STORY${n}.COM ${OVERLAY})
endforeach()
rusty_module(visual ja VISUAL.COM ${OVERLAY})
set(GAME_DIRS --game-dir "ja=${RUSTY_GAME_JA}")

# English translation: the 18 code files the patch changes (same function addresses as the Japanese files, so the
# Japanese names, coverage and entries apply); the identical files use the Japanese modules.
if(RUSTY_ENGLISH AND EXISTS "${RUSTY_UNPACKED_EN}/BOOT.COM")
    set(EN_FILE NAMES COVERAGE ENTRIES)
    rusty_module(boot en BOOT.COM ${EN_FILE} ARGS --entry 100 --entry 316 --entry 278)
    rusty_module(op en OP.COM ${EN_FILE} ARGS --entry 100)
    rusty_module(jo en JO.EXE ${EN_FILE} ARGS --exe --safe-point 00d4)
    rusty_module(grpegc en GRPEGC.COM ${OVERLAY})
    rusty_module(visual en VISUAL.COM ${OVERLAY})
    foreach(n RANGE 1 10)
        rusty_module(story${n} en STORY${n}.COM ${OVERLAY})
    endforeach()
    foreach(n IN ITEMS 4 9 10)
        rusty_module(enemy${n} en ENEMY${n}.COM HANDLERS ${OVERLAY})
    endforeach()
    list(APPEND GAME_DIRS --game-dir "en=${RUSTY_GAME_EN}")
endif()

set(LINK_TABLE "${GEN}/link.c")
add_custom_command(
    OUTPUT "${LINK_TABLE}"
    COMMAND "${RUSTY_PYTHON}" "${RECOMP}/link.py" ${RUSTY_MODULES} ${GAME_DIRS} --out "${LINK_TABLE}"
    DEPENDS "${RECOMP}/link.py" "${RECOMP}/hashing.py"
    COMMENT "Writing the module link table"
    VERBATIM)

# Missing entry points: the runtime logs code it has to interpret (build/recomp/missing.txt); this target moves the
# log into tools/recomp/entries, so the next build recompiles that code.
add_custom_target(collect-missing
    COMMAND "${RUSTY_PYTHON}" "${RECOMP}/collect_missing.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Moving the missing-address log into tools/recomp/entries"
    VERBATIM)

set(C_WARNINGS -Wall -Wextra -Werror)
set(C_OPTIONS -O2 ${C_WARNINGS})
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)

# ymfm (YM2608 emulation) and the OPNA board around it.
add_library(rusty_sound STATIC
    third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_ssg.cpp third_party/ymfm/ymfm_adpcm.cpp runtime/sound.cpp)
target_include_directories(rusty_sound PRIVATE runtime)
target_compile_options(rusty_sound PRIVATE -O2 -w)

# The generated code: compiled once for both programs.
add_library(rusty_game OBJECT ${RUSTY_SOURCES} "${LINK_TABLE}")
target_include_directories(rusty_game PRIVATE runtime)
target_compile_definitions(rusty_game PRIVATE _DEFAULT_SOURCE $<$<BOOL:${WIN32}>:__USE_MINGW_ANSI_STDIO=1>)
target_compile_options(rusty_game PRIVATE ${C_OPTIONS})

set(RUNTIME_SOURCES
    runtime/rt.c runtime/interp.c runtime/dos.c runtime/disk.c runtime/roms.c runtime/exepack.c runtime/console.c
    runtime/input.c runtime/cheats.c runtime/state.c runtime/pc98.c runtime/bios.c runtime/video.c runtime/display.c
    runtime/platform.c runtime/main.c)

# rusty_runtime(<target> [SDL]): the runtime as a static library, headless or with the SDL3 window.
function(rusty_runtime target)
    cmake_parse_arguments(PARSE_ARGV 1 R "SDL" "" "")
    set(sources ${RUNTIME_SOURCES})
    if(R_SDL)
        list(APPEND sources runtime/gpu.c)
    endif()
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC runtime)
    target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE $<$<BOOL:${RUSTY_RELEASE}>:RUSTY_RELEASE>
        $<$<BOOL:${WIN32}>:__USE_MINGW_ANSI_STDIO=1> RUSTY_VERSION="${PROJECT_VERSION}")
    target_compile_options(${target} PRIVATE ${C_OPTIONS})
    target_link_libraries(${target} PUBLIC rusty_sound)
    if(NOT WIN32)
        target_link_libraries(${target} PUBLIC m)
    endif()
    if(R_SDL)
        target_compile_definitions(${target} PRIVATE RUSTY_SDL)
        target_include_directories(${target} PRIVATE "${SHADERS}")
        target_link_libraries(${target} PUBLIC SDL3::SDL3)
        add_dependencies(${target} rusty_shaders)
    endif()
endfunction()

# Dev builds find the ROMs of the project (roms/) as "roms/ next to the program".
if(NOT RUSTY_RELEASE)
    file(CREATE_LINK "${CMAKE_SOURCE_DIR}/roms" "${CMAKE_BINARY_DIR}/roms" SYMBOLIC)
endif()

rusty_runtime(rusty_runtime_headless)
# Windows: the manifest (UTF-8 code page for file names, DPI awareness) and the version shown in the file properties.
set(PROGRAM_RESOURCES "")
if(WIN32)
    set(VERSION_NUMBERS "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    configure_file(packaging/rusty.rc.in "${CMAKE_BINARY_DIR}/rusty.rc" @ONLY)
    set(PROGRAM_RESOURCES "${CMAKE_BINARY_DIR}/rusty.rc")
endif()
add_executable(rusty_headless $<TARGET_OBJECTS:rusty_game> ${PROGRAM_RESOURCES})
target_link_libraries(rusty_headless PRIVATE rusty_runtime_headless)
set_target_properties(rusty_headless PROPERTIES LINKER_LANGUAGE CXX)
set(PROGRAMS rusty_headless)

# The window program: SDL3 and the shaders of the GPU output (GLSL -> SPIR-V as C arrays).
find_package(SDL3 CONFIG)
find_program(GLSLC glslc)
if(SDL3_FOUND AND GLSLC)
    set(SHADERS "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${SHADERS}")
    set(SHADER_OUTPUTS "")
    foreach(shader IN ITEMS fullscreen.vert decode.frag present.frag)
        add_custom_command(
            OUTPUT "${SHADERS}/${shader}.inc"
            COMMAND "${GLSLC}" -O -mfmt=c -o "${SHADERS}/${shader}.inc" "${CMAKE_SOURCE_DIR}/runtime/shaders/${shader}"
            DEPENDS "runtime/shaders/${shader}"
            VERBATIM)
        list(APPEND SHADER_OUTPUTS "${SHADERS}/${shader}.inc")
    endforeach()
    add_custom_target(rusty_shaders DEPENDS ${SHADER_OUTPUTS})
    rusty_runtime(rusty_runtime_sdl SDL)
    add_executable(rusty $<TARGET_OBJECTS:rusty_game> ${PROGRAM_RESOURCES})
    target_link_libraries(rusty PRIVATE rusty_runtime_sdl)
    set_target_properties(rusty PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32)
        # A GUI program (no console window); SDL_main provides WinMain. WinMain is in the runtime archive, but MinGW's
        # startup library (linked last) is what needs it.
        set_target_properties(rusty PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(rusty PRIVATE -Wl,-u,WinMain)
    endif()
    list(APPEND PROGRAMS rusty)
else()
    message(WARNING "SDL3 or glslc not found: only rusty_headless is built")
endif()

if(RUSTY_RELEASE)
    foreach(program IN LISTS PROGRAMS)
        if(WIN32)
            # The MinGW runtime (libgcc, libstdc++, winpthreads) linked in: only SDL3.dll is needed next to the exe.
            target_link_options(${program} PRIVATE -static)
        else()
            target_link_options(${program} PRIVATE -static-libstdc++ -static-libgcc)
            set_target_properties(${program} PROPERTIES INSTALL_RPATH "$ORIGIN/lib" BUILD_WITH_INSTALL_RPATH ON)
        endif()
    endforeach()
endif()

# Release package (distribution model A): the window program, libSDL3 in lib/, README.txt, portable.txt (portable
# mode: saves next to the program), LICENSE (MIT, this project) and licenses/ (ymfm BSD-3-Clause, compiled in; SDL3
# zlib, bundled). No game data and no ROMs.
if(TARGET rusty)
    install(TARGETS rusty DESTINATION .)
    get_target_property(SDL_LIBRARY SDL3::SDL3-shared LOCATION)
    file(REAL_PATH "${SDL_LIBRARY}" SDL_LIBRARY)
    if(WIN32)
        install(FILES "${SDL_LIBRARY}" DESTINATION .)
        install(FILES packaging/licenses/MinGW-w64-runtime-LICENSE.txt DESTINATION licenses)
    else()
        install(FILES "${SDL_LIBRARY}" DESTINATION lib RENAME libSDL3.so.0)
    endif()
    install(FILES packaging/README.txt packaging/portable.txt LICENSE DESTINATION .)
    install(FILES third_party/ymfm/LICENSE DESTINATION licenses RENAME ymfm-LICENSE.txt)
    install(FILES packaging/licenses/SDL3-LICENSE.txt DESTINATION licenses)
    # Platform names as in .NET runtime identifiers: linux-x64, win-x64, osx-arm64.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" RUSTY_ARCH)
    if(RUSTY_ARCH MATCHES "^(x86_64|amd64)$")
        set(RUSTY_ARCH x64)
    elseif(RUSTY_ARCH MATCHES "^(aarch64|arm64)$")
        set(RUSTY_ARCH arm64)
    endif()
    if(WIN32)
        set(RUSTY_OS win)
    elseif(APPLE)
        set(RUSTY_OS osx)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" RUSTY_OS)
    endif()
    set(RUSTY_PLATFORM "${RUSTY_OS}-${RUSTY_ARCH}")
    if(WIN32)
        set(CPACK_GENERATOR ZIP)
    else()
        set(CPACK_GENERATOR TGZ)
    endif()
    set(CPACK_PACKAGE_FILE_NAME "rusty-${PROJECT_VERSION}-${RUSTY_PLATFORM}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_STRIP_FILES ON)
    include(CPack)
endif()
