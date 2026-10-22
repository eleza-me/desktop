# Tests and tools of the non-GUI core.
#
# Two modes:
#   * included from the root tests (the ElezaCore target already exists);
#   * standalone: cmake -S tests/core -B build-core -G Ninja && cmake --build build-core -j2
#     which pulls lib/base and Eleza/core.cmake itself, without the GUI tree.

if (NOT TARGET ElezaCore)
	cmake_minimum_required(VERSION 3.21)
	project(ElezaCoreTests VERSION 0.1.0 LANGUAGES CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_CXX_EXTENSIONS OFF)
	if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
		set(CMAKE_BUILD_TYPE RelWithDebInfo)
	endif()
	enable_testing()
	get_filename_component(_eleza_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
	add_subdirectory("${_eleza_repo}/lib/base" "${CMAKE_BINARY_DIR}/lib_base")
	include("${_eleza_repo}/Eleza/core.cmake")
else()
	get_filename_component(_eleza_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Test)

set(_eleza_core_testdata "${CMAKE_CURRENT_SOURCE_DIR}/testdata")

function(eleza_core_test name)
	if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp")
		return()
	endif()
	add_executable(${name} ${name}.cpp ${ARGN})
	set_target_properties(${name} PROPERTIES AUTOMOC ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(${name} PRIVATE ElezaCore Qt6::Test)
	target_compile_definitions(${name} PRIVATE ELEZA_TESTDATA_DIR="${_eleza_core_testdata}")
	if (MSVC)
		target_compile_options(${name} PRIVATE /W4 /utf-8)
	else()
		target_compile_options(${name} PRIVATE -Wall -Wextra)
	endif()
	add_test(NAME ${name} COMMAND ${name})
	set_tests_properties(${name} PROPERTIES
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
		TIMEOUT 120)
	if (WIN32 AND NOT CMAKE_VERSION VERSION_LESS 3.22)
		# The Qt DLLs, without Qt's bin directory being in PATH.
		set_property(TEST ${name} APPEND PROPERTY
			ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
	endif()
endfunction()

eleza_core_test(test_base)
eleza_core_test(test_crypto)
eleza_core_test(test_noise)
eleza_core_test(test_proto)
eleza_core_test(test_storage)
eleza_core_test(test_api)
eleza_core_test(test_models)
eleza_core_test(test_group_rules)
eleza_core_test(test_groups test_fakes.h test_harness.h)
eleza_core_test(test_outbox test_fakes.h test_harness.h)
eleza_core_test(test_sync test_fakes.h test_harness.h)
eleza_core_test(test_chats test_fakes.h test_harness.h)
eleza_core_test(test_action_bar test_fakes.h test_harness.h)
eleza_core_test(test_media test_fakes.h test_harness.h)
eleza_core_test(test_bots test_fakes.h test_harness.h)
eleza_core_test(test_sticker_decode)
eleza_core_test(test_stories test_fakes.h test_harness.h)
eleza_core_test(test_notes test_fakes.h test_harness.h)
eleza_core_test(test_shared_media test_fakes.h test_harness.h)
# In-app updates (only with the whole project: the library is not a part of
# the standalone core build).
if (TARGET ElezaUpdater)
	eleza_core_test(test_updater test_fakes.h)
	target_link_libraries(test_updater PRIVATE ElezaUpdater ElezaInstall ElezaMiniz)
	set_tests_properties(test_updater PROPERTIES TIMEOUT 300)
endif()

# Command line harness against a real or mock API (see tests/core/README in the report):
#   eleza_cli --api=http://127.0.0.1:8000 --workdir=/tmp/x login <phone> <code>
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/eleza_cli.cpp")
	add_executable(eleza_cli eleza_cli.cpp)
	set_target_properties(eleza_cli PROPERTIES AUTOMOC ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(eleza_cli PRIVATE ElezaCore)
	if (MSVC)
		target_compile_options(eleza_cli PRIVATE /W4 /utf-8)
	else()
		target_compile_options(eleza_cli PRIVATE -Wall -Wextra)
	endif()
endif()

# Realtime interop: the C++ client against a peer built from the web client's own protocol code
# (tests/core/rtpeer). Skipped (exit code 77) when node / the web client sources are missing.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_rt_interop.cpp")
	eleza_core_test(test_rt_interop)
	find_program(ELEZA_NODE node)
	set(ELEZA_PWA_DIR "${_eleza_repo}/../pwa" CACHE PATH "Checkout of the web client (for the realtime peer)")
	set_tests_properties(test_rt_interop PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 300
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ELEZA_NODE=${ELEZA_NODE};ELEZA_PWA_DIR=${ELEZA_PWA_DIR};ELEZA_RTPEER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rtpeer;ELEZA_RTPEER_WORK=${CMAKE_CURRENT_BINARY_DIR}/rtpeer")
endif()
