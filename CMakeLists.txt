cmake_minimum_required(VERSION 3.28)
project(ListenFree VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(LISTENFREE_BUILD_TESTS "Build ListenFree tests" ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Qml Quick Sql Multimedia Network Concurrent Test)
qt_standard_project_setup(REQUIRES 6.10)

add_library(listenfree_domain INTERFACE)
target_include_directories(listenfree_domain INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(listenfree_domain INTERFACE cxx_std_20)

add_library(listenfree_application STATIC
    src/application/ports.h
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
target_link_libraries(listenfree_library PUBLIC listenfree_domain Qt6::Core Qt6::Concurrent)
target_include_directories(listenfree_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_media STATIC
    src/media/playback_state_machine.h
    src/media/playback_state_machine.cpp
    src/media/qt_audio_player.h
    src/media/qt_audio_player.cpp
)
target_link_libraries(listenfree_media PUBLIC listenfree_domain Qt6::Core Qt6::Multimedia)
target_include_directories(listenfree_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(listenfree_source_protocol STATIC
    src/sourcehost/source_protocol.h
    src/sourcehost/source_protocol.cpp
)
target_link_libraries(listenfree_source_protocol PUBLIC Qt6::Core)
target_include_directories(listenfree_source_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(listenfree-sourcehost src/sourcehost/sourcehost_main.cpp)
target_link_libraries(listenfree-sourcehost PRIVATE listenfree_source_protocol Qt6::Core)

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
target_link_libraries(listenfree_qmlbridge PUBLIC listenfree_application listenfree_database listenfree_library listenfree_media listenfree_online Qt6::Core Qt6::Qml)
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
    target_link_libraries(listenfree_tests PRIVATE listenfree_qmlbridge listenfree_source_protocol listenfree_source_client Qt6::Test Qt6::Sql)
    target_include_directories(listenfree_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME listenfree_tests COMMAND listenfree_tests -platform offscreen)
endif()

if(MSVC)
    target_compile_options(listenfree_domain INTERFACE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(listenfree_domain INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
