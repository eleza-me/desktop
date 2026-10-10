# Rules that run the host code generators.
#
# eleza_generate_palette(<target> <colors.palette>)
#   -> <bin>/gen/styles/palette.h/.cpp compiled into <target>.
#
# eleza_generate_styles(<target> <file.style>... [INCLUDE_DIRS dirs...])
#   -> <bin>/gen/styles/style_<name>.h/.cpp for every file, compiled into <target>.
#   `using "dir/name.style";` is resolved against INCLUDE_DIRS, the directories of the
#   listed files, and every directory registered by previous calls (other targets).
#   Usage in code: #include "styles/style_<name>.h" and st::<valueName>.
#
# eleza_generate_lang(<target> <lang.strings> <lang_en.strings>)
#   -> <bin>/gen/lang_auto.h/.cpp compiled into <target>.

define_property(GLOBAL PROPERTY ELEZA_STYLE_INCLUDE_DIRS
	BRIEF_DOCS "Directories searched for `using` in .style files"
	FULL_DOCS "Directories searched for `using` in .style files")
define_property(GLOBAL PROPERTY ELEZA_STYLE_ALL_FILES
	BRIEF_DOCS "Every .style file known to the build"
	FULL_DOCS "Every .style file known to the build")

# eleza_host_tool_command(<variable> <tool target>)
#   The command line prefix that runs a host tool during the build. The tools link
#   QtCore dynamically: on Windows Qt's bin directory has to be in PATH for them to
#   start (a build from an IDE or a plain prompt has no Qt in PATH; the failure is a
#   silent exit code 0xC0000135 from the custom command).
function(eleza_host_tool_command variable tool)
	if (CMAKE_HOST_WIN32 AND NOT CMAKE_VERSION VERSION_LESS 3.25)
		set(${variable}
			"${CMAKE_COMMAND}" -E env
			--modify "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
			"$<TARGET_FILE:${tool}>"
			PARENT_SCOPE)
	elseif (CMAKE_HOST_WIN32)
		# CMake < 3.25 has no --modify: PATH as it was when CMake configured.
		string(REPLACE ";" "\\;" path "$ENV{PATH}")
		set(${variable}
			"${CMAKE_COMMAND}" -E env
			"PATH=$<SHELL_PATH:$<TARGET_FILE_DIR:Qt6::Core>>\\;${path}"
			"$<TARGET_FILE:${tool}>"
			PARENT_SCOPE)
	else()
		set(${variable} "$<TARGET_FILE:${tool}>" PARENT_SCOPE)
	endif()
endfunction()

function(eleza_generate_palette target palette_file)
	get_filename_component(palette_abs "${palette_file}" ABSOLUTE)
	set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
	set(outputs "${gen_dir}/styles/palette.h" "${gen_dir}/styles/palette.cpp")
	eleza_host_tool_command(codegen_style_command codegen_style)
	add_custom_command(
		OUTPUT ${outputs}
		COMMAND ${codegen_style_command}
			--palette "${palette_abs}" --palette-only -o "${gen_dir}/styles"
		DEPENDS codegen_style "${palette_abs}"
		COMMENT "Generating palette (${target})"
		VERBATIM)
	set_source_files_properties(${outputs} PROPERTIES SKIP_AUTOMOC ON GENERATED ON)
	target_sources(${target} PRIVATE ${outputs})
	target_include_directories(${target} PUBLIC "${gen_dir}")
endfunction()

function(eleza_generate_styles target)
	cmake_parse_arguments(ARG "" "" "INCLUDE_DIRS" ${ARGN})
	set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
	set(files)
	foreach(file ${ARG_UNPARSED_ARGUMENTS})
		get_filename_component(abs "${file}" ABSOLUTE)
		list(APPEND files "${abs}")
	endforeach()
	foreach(dir ${ARG_INCLUDE_DIRS})
		get_filename_component(abs "${dir}" ABSOLUTE)
		set_property(GLOBAL APPEND PROPERTY ELEZA_STYLE_INCLUDE_DIRS "${abs}")
	endforeach()
	set_property(GLOBAL APPEND PROPERTY ELEZA_STYLE_ALL_FILES ${files})
	get_property(include_dirs GLOBAL PROPERTY ELEZA_STYLE_INCLUDE_DIRS)
	get_property(all_files GLOBAL PROPERTY ELEZA_STYLE_ALL_FILES)
	set(include_args)
	foreach(dir ${include_dirs})
		list(APPEND include_args "-I" "${dir}")
	endforeach()
	eleza_host_tool_command(codegen_style_command codegen_style)
	foreach(file ${files})
		get_filename_component(name "${file}" NAME_WE)
		set(outputs "${gen_dir}/styles/style_${name}.h" "${gen_dir}/styles/style_${name}.cpp")
		add_custom_command(
			OUTPUT ${outputs}
			COMMAND ${codegen_style_command}
				--palette "${ELEZA_PALETTE_FILE}"
				${include_args}
				-o "${gen_dir}/styles"
				"${file}"
			DEPENDS codegen_style "${ELEZA_PALETTE_FILE}" ${all_files}
			COMMENT "Generating style_${name} (${target})"
			VERBATIM)
		set_source_files_properties(${outputs} PROPERTIES SKIP_AUTOMOC ON GENERATED ON)
		target_sources(${target} PRIVATE ${outputs})
	endforeach()
	target_include_directories(${target} PUBLIC "${gen_dir}")
endfunction()

function(eleza_generate_lang target strings_fa strings_en)
	get_filename_component(fa_abs "${strings_fa}" ABSOLUTE)
	get_filename_component(en_abs "${strings_en}" ABSOLUTE)
	set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
	set(outputs "${gen_dir}/lang_auto.h" "${gen_dir}/lang_auto.cpp")
	eleza_host_tool_command(codegen_lang_command codegen_lang)
	add_custom_command(
		OUTPUT ${outputs}
		COMMAND ${codegen_lang_command} -o "${gen_dir}" "${fa_abs}" "${en_abs}"
		DEPENDS codegen_lang "${fa_abs}" "${en_abs}"
		COMMENT "Generating lang_auto (${target})"
		VERBATIM)
	set_source_files_properties(${outputs} PROPERTIES SKIP_AUTOMOC ON GENERATED ON)
	target_sources(${target} PRIVATE ${outputs})
	target_include_directories(${target} PUBLIC "${gen_dir}")
endfunction()
