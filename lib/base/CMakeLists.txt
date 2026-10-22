# ElezaBase: small non-GUI helpers shared by every target (QtCore only).
# Works both from the root project and from tests/core as a standalone build.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core)

add_library(ElezaBase STATIC
	basic_types.h
	backoff.h
	bytes.cpp
	bytes.h
	expected.h
	platform_file.cpp
	platform_file.h
	random.cpp
	random.h
	text_direction.cpp
	text_direction.h
	timer.cpp
	timer.h
	unixtime.cpp
	unixtime.h
	weak_guard.h
)
set_target_properties(ElezaBase PROPERTIES
	AUTOMOC ON
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)
# Headers are included as "base/<name>.h".
target_include_directories(ElezaBase PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ElezaBase PUBLIC Qt6::Core)
if (MSVC)
	target_compile_options(ElezaBase PRIVATE /W4 /utf-8)
else()
	target_compile_options(ElezaBase PRIVATE -Wall -Wextra)
endif()
