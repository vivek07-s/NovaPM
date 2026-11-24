cmake_minimum_required(VERSION 3.16)
project(NovaPM VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NOVAPM_BUILD_GUI   "Build the Dear ImGui desktop application (needs OpenGL; downloads ImGui + GLFW)" ON)
option(NOVAPM_BUILD_TESTS "Build the unit / end-to-end tests" ON)
option(NOVAPM_SYSTEM_SQLITE "Use the system SQLite library when it is available" ON)

include(FetchContent)

# ---------------------------------------------------------------- SQLite
set(NOVAPM_SQLITE_TARGET "")
if(NOVAPM_SYSTEM_SQLITE)
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    set(NOVAPM_SQLITE_TARGET SQLite::SQLite3)
  endif()
endif()
if(NOT NOVAPM_SQLITE_TARGET)
  FetchContent_Declare(sqlite URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sqlite)
  add_library(sqlite3_lib STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3_lib PUBLIC ${sqlite_SOURCE_DIR})
  set(NOVAPM_SQLITE_TARGET sqlite3_lib)
endif()

# ---------------------------------------------------------------- core library (no GUI)
if(WIN32)
  set(NOVAPM_PLATFORM_SRC src/platform/win32/process_collector.cpp src/platform/win32/system_monitor.cpp src/platform/win32/signal_manager.cpp)
else()
  set(NOVAPM_PLATFORM_SRC src/platform/linux/process_collector.cpp src/platform/linux/system_monitor.cpp src/platform/linux/signal_manager.cpp)
endif()

add_library(novapm_core STATIC
  ${NOVAPM_PLATFORM_SRC}
  src/container.cpp src/database.cpp src/threshold_guard.cpp src/config.cpp src/monitor_core.cpp src/cli.cpp)
target_include_directories(novapm_core PUBLIC src)
target_link_libraries(novapm_core PUBLIC ${NOVAPM_SQLITE_TARGET})
if(WIN32)
  target_link_libraries(novapm_core PUBLIC psapi advapi32 user32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(novapm_core PUBLIC Threads::Threads)
endif()
if(MSVC)
  target_compile_options(novapm_core PUBLIC /W3 /utf-8 /permissive-)
  target_compile_definitions(novapm_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(novapm_core PUBLIC -Wall -Wextra)
endif()

# ---------------------------------------------------------------- command-line tool
add_executable(novapm-cli src/main_cli.cpp)
target_link_libraries(novapm-cli PRIVATE novapm_core)
set(NOVAPM_INSTALL_TARGETS novapm-cli)

# ---------------------------------------------------------------- GUI
if(NOVAPM_BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "X11 only by default; runs under Wayland through XWayland" FORCE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.4-docking GIT_SHALLOW TRUE)
  FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git     GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui glfw)
  find_package(OpenGL REQUIRED)

  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

  add_executable(novapm src/main.cpp)
  target_link_libraries(novapm PRIVATE novapm_core imgui_lib)
  list(APPEND NOVAPM_INSTALL_TARGETS novapm)
endif()

# ---------------------------------------------------------------- tests
if(NOVAPM_BUILD_TESTS)
  enable_testing()
  add_executable(guard_test tests/guard_test.cpp)
  target_link_libraries(guard_test PRIVATE novapm_core)
  add_test(NAME guard_test COMMAND guard_test)

  add_executable(core_test tests/core_test.cpp)
  target_link_libraries(core_test PRIVATE novapm_core)
  add_test(NAME core_test COMMAND core_test)

  add_executable(load_gen tests/load_gen.cpp)
  if(UNIX)
    add_test(NAME e2e_guard
             COMMAND ${CMAKE_SOURCE_DIR}/tests/e2e_linux.sh $<TARGET_FILE:novapm-cli> $<TARGET_FILE:load_gen>)
  endif()
endif()

# ---------------------------------------------------------------- install + packages (cpack)
install(TARGETS ${NOVAPM_INSTALL_TARGETS} RUNTIME DESTINATION bin)
install(FILES README.md packaging/novapm.ini.sample DESTINATION share/doc/novapm)

set(CPACK_PACKAGE_NAME "novapm")
set(CPACK_PACKAGE_VENDOR "NovaTech")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Process and resource monitor with persistent history and a usage threshold guard")
set(CPACK_PACKAGE_CONTACT "NovaTech <24021036@geu.ac.in>")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR "TGZ;DEB")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION utils)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
endif()
include(CPack)
