# eleza_target_defaults(<target>)
#   Warning level and common definitions for our own targets.
function(eleza_target_defaults target)
	if (MSVC)
		target_compile_options(${target} PRIVATE /W4 /wd4127 /wd4458 /wd4456 /wd4457 /wd4702)
		if (ELEZA_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-sign-compare)
		if (ELEZA_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
	target_compile_definitions(${target} PRIVATE
		QT_NO_FOREACH
		QT_DISABLE_DEPRECATED_BEFORE=0x060400)
endfunction()

# eleza_add_resources(<target> <qrc>... [DEPENDS files...])
#   Compiles .qrc files with rcc into the target (works for static libraries too;
#   the library must call Q_INIT_RESOURCE(<qrc base name>) once).
function(eleza_add_resources target)
	cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
	foreach(qrc ${ARG_UNPARSED_ARGUMENTS})
		get_filename_component(qrc_abs "${qrc}" ABSOLUTE)
		get_filename_component(qrc_name "${qrc}" NAME_WE)
		set(out "${CMAKE_CURRENT_BINARY_DIR}/gen/qrc_${qrc_name}.cpp")
		add_custom_command(
			OUTPUT "${out}"
			COMMAND Qt6::rcc --name "${qrc_name}" --output "${out}" "${qrc_abs}"
			DEPENDS "${qrc_abs}" ${ARG_DEPENDS}
			COMMENT "rcc ${qrc_name}.qrc"
			VERBATIM)
		set_source_files_properties("${out}" PROPERTIES SKIP_AUTOMOC ON GENERATED ON)
		target_sources(${target} PRIVATE "${out}")
	endforeach()
endfunction()
