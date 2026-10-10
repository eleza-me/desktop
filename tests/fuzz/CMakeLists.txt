# Fuzz targets (-DELEZA_BUILD_FUZZERS=ON, off by default). See fuzz_driver.h.
#
#   cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug -DELEZA_BUILD_FUZZERS=ON \
#     -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
#   ninja -C build-fuzz fuzzers
#   ./build-fuzz/tests/fuzz/fuzz_proto -max_total_time=300
#
# ELEZA_FUZZ_ENGINE:
#   driver     (default) tests/fuzz/fuzz_driver.cpp: seeds + random mutations, any compiler;
#   libfuzzer  clang's -fsanitize=fuzzer (coverage guided) - needs the libFuzzer runtime.
# Sanitizers are not added here: pass them in CMAKE_CXX_FLAGS for the whole
# tree, so the code under test is instrumented too.

set(ELEZA_FUZZ_ENGINE "driver" CACHE STRING "Fuzz engine: driver or libfuzzer")
set_property(CACHE ELEZA_FUZZ_ENGINE PROPERTY STRINGS driver libfuzzer)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui)

get_filename_component(_eleza_fuzz_src "${CMAKE_CURRENT_SOURCE_DIR}/../../Eleza/SourceFiles" ABSOLUTE)

add_custom_target(fuzzers)

# eleza_add_fuzzer(<name> <source> [LIBS libs...] [SOURCES extra...])
function(eleza_add_fuzzer name source)
	cmake_parse_arguments(ARG "" "" "LIBS;SOURCES" ${ARGN})
	add_executable(${name} EXCLUDE_FROM_ALL ${source} ${ARG_SOURCES})
	set_target_properties(${name} PROPERTIES AUTOMOC OFF)
	target_include_directories(${name} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}"
		"${_eleza_fuzz_src}")
	target_link_libraries(${name} PRIVATE Qt6::Core ${ARG_LIBS})
	eleza_target_defaults(${name})
	if (ELEZA_FUZZ_ENGINE STREQUAL "libfuzzer")
		target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
		target_link_options(${name} PRIVATE -fsanitize=fuzzer)
	else()
		target_sources(${name} PRIVATE fuzz_driver.cpp)
	endif()
	add_dependencies(fuzzers ${name})
endfunction()

if (TARGET ElezaCore)
	eleza_add_fuzzer(fuzz_proto fuzz_proto.cpp LIBS ElezaCore)
	eleza_add_fuzzer(fuzz_frames fuzz_frames.cpp LIBS ElezaCore)
	eleza_add_fuzzer(fuzz_models fuzz_models.cpp LIBS ElezaCore)
	eleza_add_fuzzer(fuzz_media fuzz_media.cpp LIBS ElezaCore ElezaBase Qt6::Gui)
endif()
eleza_add_fuzzer(fuzz_text fuzz_text.cpp LIBS ElezaUi Qt6::Gui)
eleza_add_fuzzer(fuzz_links fuzz_links.cpp LIBS ElezaUi Qt6::Gui
	SOURCES
		"${_eleza_fuzz_src}/support/links_parse.cpp"
		"${_eleza_fuzz_src}/window/window_routes.cpp")
