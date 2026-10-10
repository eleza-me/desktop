# Third-party code that is built from source with the project.
#
# miniz 3.0.2 (https://github.com/richgel999/miniz, MIT licence, see
# miniz/LICENSE): the amalgamated miniz.c / miniz.h of the release archive,
# unchanged. It reads the update packages (zip). Only what that needs is
# compiled in: no archive writing, no stdio (the app reads through its own
# file callbacks), no zlib-compatible names (Qt brings the real zlib).

add_library(ElezaMiniz STATIC
	miniz/miniz.c
	miniz/miniz.h)
set_target_properties(ElezaMiniz PROPERTIES
	AUTOMOC OFF
	C_STANDARD 99)
target_include_directories(ElezaMiniz SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz")
target_compile_definitions(ElezaMiniz PUBLIC
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
	MINIZ_NO_ARCHIVE_WRITING_APIS
	MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if (MSVC)
	target_compile_options(ElezaMiniz PRIVATE /w)
else()
	target_compile_options(ElezaMiniz PRIVATE -w)
endif()
