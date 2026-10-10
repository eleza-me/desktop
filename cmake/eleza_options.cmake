# Build options and global compiler setup shared by every target.

option(ELEZA_BUILD_TESTS "Build the unit tests (ctest)." ON)
option(ELEZA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors." OFF)
option(ELEZA_BUILD_FUZZERS "Build the fuzz targets (tests/fuzz)." OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# Scripted input for screenshots (--debug-actions click / type / key steps) calls two
# functions QtGui exports for QtTest only. Off: those steps are refused with a warning;
# --debug-grab, --debug-route and the wait / grab / route / resize steps keep working.
if (MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(_eleza_debug_tools_default OFF)
else()
	set(_eleza_debug_tools_default ON)
endif()
option(ELEZA_DEBUG_TOOLS
	"Scripted mouse / keyboard input (--debug-actions) through QtGui's test entry points."
	${_eleza_debug_tools_default})

if (MSVC)
	# Sources (and generated strings) are UTF-8 with Persian literals. The options are
	# for the C++ compiler only: the same list would reach rc.exe (Eleza.rc), which
	# stops on the first option it does not know.
	add_compile_options(
		"$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/permissive-;/Zc:__cplusplus;/bigobj;/MP>")
endif()
if (WIN32)
	add_compile_definitions(
		UNICODE _UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		_USE_MATH_DEFINES
		_CRT_SECURE_NO_WARNINGS)
endif()
