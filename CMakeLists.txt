cmake_minimum_required(VERSION 3.24...3.30)
project(raylib-game-template LANGUAGES C CXX)

include(FetchContent)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
set(RAYLIB_VERSION 6.0)

FetchContent_Declare(
    raylib
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz
    FIND_PACKAGE_ARGS
)

# Box2D v3: C17 rigid-body physics (desktop and Emscripten/WebAssembly). Keep
# the dependency lean; the project supplies its own rendering and tooling.
set(BOX2D_SAMPLES OFF CACHE BOOL "Build Box2D samples" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "Build Box2D unit tests" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "Build Box2D documentation" FORCE)
FetchContent_Declare(
    box2d
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/erincatto/box2d/archive/refs/tags/v3.1.1.tar.gz
)

# cimgui: C bindings for Dear ImGui (bundles imgui as a submodule)
set(IMGUI_STATIC "yes" CACHE STRING "Build cimgui as a static library" FORCE)
FetchContent_Declare(
    cimgui
    GIT_REPOSITORY https://github.com/cimgui/cimgui.git
    GIT_TAG d298666861ebf00dcfeb2407409931c04e47e33c
)

# rlImGui: raylib backend for Dear ImGui (no CMakeLists, built below)
FetchContent_Declare(
    rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG ef129d1858373b6fe332c45f85a1dfc1421bebae
)

FetchContent_MakeAvailable(raylib box2d cimgui rlimgui)

add_library(rlimgui_lib STATIC ${rlimgui_SOURCE_DIR}/rlImGui.cpp)
target_include_directories(rlimgui_lib PUBLIC ${rlimgui_SOURCE_DIR})
target_link_libraries(rlimgui_lib PUBLIC cimgui raylib)
target_compile_definitions(rlimgui_lib PUBLIC NO_FONT_AWESOME)

# Our Project
add_executable(${PROJECT_NAME})
add_subdirectory(src)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PROJECT_NAME})

set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# Build id baked into the binary: the short git hash at configure time (CI
# configures fresh per commit, so the Pages build always carries its commit)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_COMMIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "nogit")
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE BUILD_GIT_HASH="${GIT_COMMIT_HASH}")

# Pull card exports from the sandbox into src/resources/models before the
# resource copy, so a fresh export only needs a recompile
add_custom_command(
    TARGET ${PROJECT_NAME} PRE_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DEXPORT_DIR=${CMAKE_SOURCE_DIR}/sandbox/export
        -DMODELS_DIR=${CMAKE_SOURCE_DIR}/src/resources/models
        -P ${CMAKE_SOURCE_DIR}/cmake/sync_card_exports.cmake
)

if ("${PLATFORM}" STREQUAL "Web")
    add_custom_command(
        TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/../resources
    )
    #DEPENDS ${PROJECT_NAME}
else()
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources
    )
    #DEPENDS ${PROJECT_NAME}
endif()

#set(raylib_VERBOSE 1)
target_link_libraries(${PROJECT_NAME} raylib rlimgui_lib box2d)

# Web Configurations
if ("${PLATFORM}" STREQUAL "Web")
    # Tell Emscripten to build an example.html file.
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
    target_link_options(${PROJECT_NAME} PUBLIC -sUSE_GLFW=3 PUBLIC --preload-file resources PUBLIC -sALLOW_MEMORY_GROWTH=1 PUBLIC --shell-file ${CMAKE_SOURCE_DIR}/src/minshell.html)
endif()

# Checks if OSX and links appropriate frameworks (Only required on MacOS)
if (APPLE)
    target_link_libraries(${PROJECT_NAME} "-framework IOKit")
    target_link_libraries(${PROJECT_NAME} "-framework Cocoa")
    target_link_libraries(${PROJECT_NAME} "-framework OpenGL")
endif()
