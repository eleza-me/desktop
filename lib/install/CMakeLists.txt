# ElezaInstall — replacing the files of an installed copy with a new version
# (install/install_swap.h). Plain C++20, no Qt: the same sources are the body
# of the Windows updater, which must not load any DLL of the app.
#
# Updater (Windows only) — Updater.exe, shipped next to Eleza.exe. Static C++
# runtime; its sources are compiled into it directly, because the library
# above uses the runtime DLL like the rest of the project.

set(_eleza_install_sources
	install/install_file_ops.cpp
	install/install_file_ops.h
	install/install_helper.cpp
	install/install_helper.h
	install/install_paths.cpp
	install/install_paths.h
	install/install_swap.cpp
	install/install_swap.h
)

function(eleza_install_target_options target)
	set_target_properties(${target} PROPERTIES
		AUTOMOC OFF
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)
	target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
	if (MSVC)
		target_compile_options(${target} PRIVATE /W4)
		if (ELEZA_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
		if (ELEZA_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()

add_library(ElezaInstall STATIC ${_eleza_install_sources})
eleza_install_target_options(ElezaInstall)

if (WIN32)
	add_executable(Updater WIN32
		updater_win/updater_main.cpp
		${_eleza_install_sources})
	eleza_install_target_options(Updater)
	set_target_properties(Updater PROPERTIES
		OUTPUT_NAME "Updater"
		# /MT: no vcruntime140.dll / msvcp140.dll next to it is needed.
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
		# Next to Eleza.exe, where the app looks for it.
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Eleza")
	target_compile_definitions(Updater PRIVATE UNICODE _UNICODE)
	target_link_libraries(Updater PRIVATE shell32 ole32)
	if (MINGW)
		target_link_options(Updater PRIVATE -municode -static)
	endif()
endif()
