cmake_minimum_required(VERSION 3.21)

project(Eleza
	VERSION 0.1.0
	DESCRIPTION "Eleza Desktop"
	HOMEPAGE_URL "https://eleza.me"
	LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(eleza_options)
include(eleza_target)
include(eleza_codegen)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Svg Network)

set(ELEZA_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Eleza/Resources")
set(ELEZA_PALETTE_FILE "${ELEZA_RESOURCES_DIR}/colors.palette")

add_subdirectory(codegen)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/base/CMakeLists.txt")
	add_subdirectory(lib/base)
endif()
add_subdirectory(lib/ui)
add_subdirectory(Eleza)

if (ELEZA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if (ELEZA_BUILD_FUZZERS)
	add_subdirectory(tests/fuzz)
endif()
