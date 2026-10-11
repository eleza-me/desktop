cmake_minimum_required(VERSION 3.21)

project(Eleza
	VERSION 0.3.0
	DESCRIPTION "Eleza Desktop"
	HOMEPAGE_URL "https://eleza.me"
	LANGUAGES C CXX)

# Only for tests/tools/update-e2e.sh, which needs "the next version" built from
# the same sources to update to. Never set for anything that is shipped: the
# release workflow compares what the binary reports with the tag.
set(ELEZA_TEST_VERSION "" CACHE STRING
	"Build as another version (x.y.z), for the update end-to-end test only.")
if (ELEZA_TEST_VERSION)
	if (NOT ELEZA_TEST_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
		message(FATAL_ERROR "ELEZA_TEST_VERSION must be x.y.z, got \"${ELEZA_TEST_VERSION}\".")
	endif()
	set(PROJECT_VERSION "${ELEZA_TEST_VERSION}")
	set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
	set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
	set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
	message(STATUS "Building as test version ${PROJECT_VERSION} (ELEZA_TEST_VERSION).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(eleza_options)
include(eleza_target)
include(eleza_codegen)
include(eleza_livekit)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Svg Network)

set(ELEZA_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Eleza/Resources")
set(ELEZA_PALETTE_FILE "${ELEZA_RESOURCES_DIR}/colors.palette")

add_subdirectory(codegen)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/base/CMakeLists.txt")
	add_subdirectory(lib/base)
endif()
add_subdirectory(lib/ui)
add_subdirectory(lib/install)
add_subdirectory(third_party)
add_subdirectory(Eleza)

if (ELEZA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if (ELEZA_BUILD_FUZZERS)
	add_subdirectory(tests/fuzz)
endif()
