cmake_minimum_required(VERSION 3.20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(FangCunIMEWindows VERSION 0.6.7 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FANGCUN_BUILD_TESTS "Build core and keyhandler unit tests" ON)
option(FANGCUN_BUILD_TSF "Build Windows TSF COM DLL (requires Windows)" ${WIN32})

# -----------------------------------------------------------------------------
# Dependencies: SQLite3 + Zlib
# -----------------------------------------------------------------------------
if(WIN32)
    include(FetchContent)
    # SQLite3 amalgamation
    FetchContent_Declare(
        sqlite3_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    )
    FetchContent_GetProperties(sqlite3_amalgamation)
    if(NOT sqlite3_amalgamation_POPULATED)
        FetchContent_Populate(sqlite3_amalgamation)
    endif()
    add_library(sqlite3_static STATIC
        "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c"
    )
    target_include_directories(sqlite3_static PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")
    target_compile_definitions(sqlite3_static PRIVATE SQLITE_THREADSAFE=1)
    add_library(SQLite::SQLite3 ALIAS sqlite3_static)

    # Zlib for decompressing pinyin-v1.dbz on first run if needed
    FetchContent_Declare(
        zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    )
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    if(TARGET zlibstatic)
        add_library(FANGCUN_ZLIB ALIAS zlibstatic)
        target_include_directories(zlibstatic PUBLIC
            "${zlib_SOURCE_DIR}"
            "${zlib_BINARY_DIR}"
        )
    else()
        add_library(FANGCUN_ZLIB ALIAS zlib)
    endif()
else()
    find_package(SQLite3 REQUIRED)
    find_package(ZLIB REQUIRED)
    add_library(FANGCUN_ZLIB ALIAS ZLIB::ZLIB)
endif()

# -----------------------------------------------------------------------------
# Core Engine Library (cross-platform C++20)
# -----------------------------------------------------------------------------
add_library(fangcun_core STATIC
    core/src/InputScheme.cpp
    core/src/CandidateRanker.cpp
    core/src/FlypyConverter.cpp
    core/src/MixedInput.cpp
    core/src/DictionaryRepository.cpp
    core/src/UserStore.cpp
    core/src/FangCunEngine.cpp
)

target_include_directories(fangcun_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
)

target_link_libraries(fangcun_core PUBLIC
    SQLite::SQLite3
    FANGCUN_ZLIB
)

if(MSVC)
    target_compile_options(fangcun_core PRIVATE /W4 /utf-8)
    target_compile_definitions(fangcun_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(fangcun_core PRIVATE -Wall -Wextra -Wpedantic)
    find_package(Threads REQUIRED)
    target_link_libraries(fangcun_core PUBLIC Threads::Threads)
endif()

# -----------------------------------------------------------------------------
# Windows TSF COM DLL
# -----------------------------------------------------------------------------
if(FANGCUN_BUILD_TSF)
    if(NOT WIN32)
        message(FATAL_ERROR "FANGCUN_BUILD_TSF is only supported on Windows.")
    endif()

    add_library(FangCunTSF SHARED
        tsf/src/DllMain.cpp
        tsf/src/Register.cpp
        tsf/src/TextService.cpp
        tsf/src/EditSession.cpp
        tsf/src/KeyHandler.cpp
        tsf/src/CandidateWindow.cpp
        tsf/src/SettingsStore.cpp
        tsf/src/SettingsWindow.cpp
        tsf/src/CustomSkinCache.cpp
        tsf/src/FangCunTSF.def
    )

    target_include_directories(FangCunTSF PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tsf/include"
    )

    target_compile_definitions(FangCunTSF PRIVATE
        FANGCUN_PROJECT_VERSION="${PROJECT_VERSION}"
    )

    target_link_libraries(FangCunTSF PRIVATE
        fangcun_core
        ole32
        oleaut32
        uuid
        advapi32
        user32
        gdi32
        d2d1
        dwrite
        shlwapi
        shell32
        windowscodecs
    )

    if(MSVC)
        target_compile_options(FangCunTSF PRIVATE /W4 /utf-8)
        set_target_properties(FangCunTSF PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Unit & Integration Tests
# -----------------------------------------------------------------------------
if(FANGCUN_BUILD_TESTS)
    enable_testing()

    set(FANGCUN_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

    add_executable(fangcun_tests
        tests/test_engine.cpp
        tests/test_keyhandler.cpp
        tsf/src/KeyHandler.cpp
    )
    target_include_directories(fangcun_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tsf/include"
    )
    target_link_libraries(fangcun_tests PRIVATE fangcun_core)
    target_compile_definitions(fangcun_tests PRIVATE
        FANGCUN_ASSETS_DIR="${FANGCUN_ASSETS_DIR}"
    )
    if(MSVC)
        target_compile_options(fangcun_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(fangcun_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME fangcun_core_and_keyhandler_tests COMMAND fangcun_tests)

    add_executable(fangcun_settings_tests tests/test_settings_ini.cpp)
    target_include_directories(fangcun_settings_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
    if(MSVC)
        target_compile_options(fangcun_settings_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(fangcun_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME fangcun_settings_merge_tests COMMAND fangcun_settings_tests)

    add_executable(fangcun_resolved_skin_tests tests/test_resolved_skin.cpp)
    target_include_directories(fangcun_resolved_skin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
    if(MSVC)
        target_compile_options(fangcun_resolved_skin_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(fangcun_resolved_skin_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME fangcun_resolved_skin_tests COMMAND fangcun_resolved_skin_tests)

    add_executable(fangcun_builtin_skin_style_tests tests/test_builtin_skin_style.cpp)
    target_include_directories(fangcun_builtin_skin_style_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
    if(MSVC)
        target_compile_options(fangcun_builtin_skin_style_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(fangcun_builtin_skin_style_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME fangcun_builtin_skin_style_tests COMMAND fangcun_builtin_skin_style_tests)

    if(WIN32)
        add_executable(fangcun_atomic_file_tests tests/test_atomic_file_windows.cpp)
        target_include_directories(fangcun_atomic_file_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tsf/include")
        target_compile_definitions(fangcun_atomic_file_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(fangcun_atomic_file_tests PRIVATE /W4 /utf-8)
        endif()
        add_test(NAME fangcun_windows_atomic_file_tests COMMAND fangcun_atomic_file_tests)

        add_executable(fangcun_custom_skin_tests tests/test_custom_skin_windows.cpp tsf/src/CustomSkinCache.cpp tsf/src/CandidateWindow.cpp)
        target_include_directories(fangcun_custom_skin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tsf/include" "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
        target_compile_definitions(fangcun_custom_skin_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(fangcun_custom_skin_tests PRIVATE ole32 windowscodecs user32 uuid d2d1 dwrite gdi32)
        if(MSVC)
            target_compile_options(fangcun_custom_skin_tests PRIVATE /W4 /utf-8)
        endif()
        add_test(NAME fangcun_windows_custom_skin_tests COMMAND fangcun_custom_skin_tests)
    endif()
endif()
