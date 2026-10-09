# Build options and global compiler setup shared by every target.

option(ELEZA_BUILD_TESTS "Build the unit tests (ctest)." ON)
option(ELEZA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors." OFF)

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

if (MSVC)
	# Sources (and generated strings) are UTF-8 with Persian literals.
	add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP)
	add_compile_definitions(
		UNICODE _UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		_USE_MATH_DEFINES
		_CRT_SECURE_NO_WARNINGS)
endif()
