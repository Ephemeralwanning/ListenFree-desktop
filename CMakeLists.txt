cmake_minimum_required(VERSION 3.28)
project(ListenFree VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(LISTENFREE_BUILD_TESTS "Build ListenFree tests" ON)
option(LISTENFREE_BUILD_MPV_PROBE "Build the optional libmpv capability probe" OFF)
set(LISTENFREE_MPV_ROOT "" CACHE PATH "Root of an extracted libmpv development package")

find_package(Qt6 6.11.2 REQUIRED COMPONENTS Core Gui Qml Quick Sql Multimedia Network Concurrent Test)
find_package(TagLib 2.3.1 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
qt_standard_project_setup(REQUIRES 6.11)

add_library(listenfree_domain INTERFACE)
target_include_directories(listenfree_domain INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(listenfree_domain INTERFACE cxx_std_20)

add_library(listenfree_application STATIC
    src/application/ports.h
    src/application/playback_service.h
    src/application/playback_service.cpp
    src/application/application_facade.h
    src/application/application_facade.cpp
)
target_link_libraries(listenfree_application PUBLIC listenfree_domain Qt6::Core)
target_include_directories(listenfree_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_database STATIC
    src/infrastructure/database/database.h
    src/infrastructure/database/database.cpp
    src/infrastructure/database/repositories.h
    src/infrastructure/database/repositories.cpp
)
target_link_libraries(listenfree_database PUBLIC listenfree_domain Qt6::Core Qt6::Sql)
target_include_directories(listenfree_database PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_library STATIC
    src/infrastructure/library/library_scanner.h
    src/infrastructure/library/library_scanner.cpp
)
target_link_libraries(listenfree_library PUBLIC listenfree_domain Qt6::Core Qt6::Concurrent PRIVATE TagLib::tag)
target_include_directories(listenfree_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_media STATIC
    src/media/playback_state_machine.h
    src/media/playback_state_machine.cpp
    src/media/qt_audio_player.h
    src/media/qt_audio_player.cpp
)
target_link_libraries(listenfree_media PUBLIC listenfree_domain Qt6::Core PRIVATE Qt6::Multimedia)
target_include_directories(listenfree_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_source_protocol STATIC
    src/sourcehost/source_protocol.h
    src/sourcehost/source_protocol.cpp
)
target_link_libraries(listenfree_source_protocol PUBLIC Qt6::Core)
target_include_directories(listenfree_source_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_plugin_runtime STATIC
    src/sourcehost/plugin_runtime.h
    src/sourcehost/plugin_runtime.cpp
)
target_link_libraries(listenfree_plugin_runtime PUBLIC listenfree_source_protocol Qt6::Core Qt6::Qml Qt6::Network ZLIB::ZLIB)
target_include_directories(listenfree_plugin_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(listenfree_plugin_runtime PRIVATE bcrypt)
endif()

add_executable(listenfree-sourcehost src/sourcehost/sourcehost_main.cpp)
target_link_libraries(listenfree-sourcehost PRIVATE listenfree_plugin_runtime Qt6::Core)

# Local-only, non-destructive scanner experiment. It defaults to an in-memory
# database and is intentionally separate from the GUI executable.
add_executable(listenfree-library-scan-probe tools/library_scan_probe.cpp)
target_link_libraries(listenfree-library-scan-probe PRIVATE listenfree_library listenfree_database Qt6::Core Qt6::Sql)
target_include_directories(listenfree-library-scan-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(LISTENFREE_BUILD_MPV_PROBE)
    if(NOT LISTENFREE_MPV_ROOT)
        message(FATAL_ERROR "LISTENFREE_MPV_ROOT is required when LISTENFREE_BUILD_MPV_PROBE=ON")
    endif()
    add_executable(listenfree-mpv-core-probe tools/mpv_core_probe.cpp)
    target_include_directories(listenfree-mpv-core-probe PRIVATE ${LISTENFREE_MPV_ROOT}/include)
    target_link_libraries(listenfree-mpv-core-probe PRIVATE
        ${LISTENFREE_MPV_ROOT}/libmpv.dll.a Qt6::Core)
    add_custom_command(TARGET listenfree-mpv-core-probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LISTENFREE_MPV_ROOT}/libmpv-2.dll
            $<TARGET_FILE_DIR:listenfree-mpv-core-probe>/libmpv-2.dll)
endif()

add_library(listenfree_source_client STATIC
    src/sourcehost/sourcehost_client.h
    src/sourcehost/sourcehost_client.cpp
)
target_link_libraries(listenfree_source_client PUBLIC listenfree_domain Qt6::Core)
target_include_directories(listenfree_source_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_online STATIC
    src/online/mock_online_provider.h
    src/online/mock_online_provider.cpp
)
target_link_libraries(listenfree_online PUBLIC listenfree_domain Qt6::Core)
target_include_directories(listenfree_online PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_qmlbridge STATIC
    src/qmlbridge/list_models.h
    src/qmlbridge/list_models.cpp
    src/qmlbridge/controllers.h
    src/qmlbridge/controllers.cpp
)
target_link_libraries(listenfree_qmlbridge
    PUBLIC listenfree_application listenfree_database listenfree_library listenfree_online Qt6::Core Qt6::Qml
    PRIVATE listenfree_media
)
target_include_directories(listenfree_qmlbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

qt_add_executable(listenfree WIN32 src/app/main.cpp)
qt_add_qml_module(listenfree
    URI ListenFree.Bootstrap
    VERSION 1.0
    QML_FILES ui/bootstrap/Main.qml
)
target_link_libraries(listenfree PRIVATE listenfree_qmlbridge Qt6::Gui Qt6::Quick Qt6::Qml)

if(LISTENFREE_BUILD_TESTS)
    enable_testing()
    add_executable(listenfree_tests tests/backend_tests.cpp)
    target_link_libraries(listenfree_tests PRIVATE listenfree_qmlbridge listenfree_source_protocol listenfree_source_client TagLib::tag Qt6::Test Qt6::Sql Qt6::Network)
    target_include_directories(listenfree_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME listenfree_tests COMMAND listenfree_tests)
    add_executable(listenfree_playback_tests tests/playback_tests.cpp)
    target_link_libraries(listenfree_playback_tests PRIVATE listenfree_media listenfree_application Qt6::Test Qt6::Network Qt6::Multimedia)
    target_include_directories(listenfree_playback_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME listenfree_playback_tests COMMAND listenfree_playback_tests -o -,txt)
    set_tests_properties(listenfree_playback_tests PROPERTIES TIMEOUT 240)
    add_executable(listenfree_playback_service_tests tests/playback_service_tests.cpp)
    target_link_libraries(listenfree_playback_service_tests PRIVATE listenfree_qmlbridge Qt6::Test)
    target_include_directories(listenfree_playback_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME listenfree_playback_service_tests COMMAND listenfree_playback_service_tests)
    add_executable(listenfree-sourcehost-fault-host tests/sourcehost_fault_host.cpp)
    target_link_libraries(listenfree-sourcehost-fault-host PRIVATE listenfree_source_protocol Qt6::Core)
    target_include_directories(listenfree-sourcehost-fault-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(listenfree_sourcehost_fault_tests tests/sourcehost_fault_tests.cpp)
    target_link_libraries(listenfree_sourcehost_fault_tests PRIVATE listenfree_source_client listenfree_source_protocol Qt6::Test)
    target_include_directories(listenfree_sourcehost_fault_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_dependencies(listenfree_sourcehost_fault_tests listenfree-sourcehost-fault-host)
    add_test(NAME listenfree_sourcehost_fault_tests COMMAND listenfree_sourcehost_fault_tests)
    set_tests_properties(listenfree_sourcehost_fault_tests PROPERTIES TIMEOUT 60)
    add_test(NAME listenfree_smoke COMMAND listenfree --smoke -platform offscreen)
    set_tests_properties(listenfree_smoke PROPERTIES TIMEOUT 10)
endif()

if(MSVC)
    target_compile_options(listenfree_domain INTERFACE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(listenfree_domain INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
