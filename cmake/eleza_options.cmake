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

# Every test / debug surface of the app is behind ELEZA_DEBUG_TOOLS: the --debug-*
# flags, --gallery, the "debug/…" routes, the ELEZA_DEBUG_* environment hooks, clear
# text API addresses outside the loopback interface and request paths in the log.
# Off: all of it is compiled out. The default is ON for a Debug build and for a build
# without a build type (a developer's plain `cmake -S . -B build`), OFF for Release,
# RelWithDebInfo and MinSizeRel on every platform, and OFF for multi-config
# generators (one cache serves Debug and Release there): what is shipped never has it.
if (CMAKE_CONFIGURATION_TYPES)
	set(_eleza_debug_tools_default OFF)
elseif (NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(_eleza_debug_tools_default ON)
else()
	set(_eleza_debug_tools_default OFF)
endif()
option(ELEZA_DEBUG_TOOLS
	"Debug / test hooks: --debug-* flags, --gallery, debug/ routes, verbose request logs."
	${_eleza_debug_tools_default})
if (ELEZA_DEBUG_TOOLS)
	# For every target: the libraries have hooks of their own (fonts, logs).
	add_compile_definitions(ELEZA_DEBUG_TOOLS)
endif()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

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
