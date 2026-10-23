# Unit tests (QtTest, registered with CTest):  ctest --test-dir <build> --output-on-failure
#
#   tests/ui/    style codegen, lang codegen, lang formatting (Jalali, digits), Ui::Text
#   tests/core/  the non-GUI core (added when present)
#   tests/app/   widget-free parts of the application: routes, error texts

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Test)

# eleza_test_runtime_path(<name>)
#   Windows: the test executables need the Qt DLLs; ctest finds them without Qt's bin
#   directory being in PATH.
function(eleza_test_runtime_path name)
	if (WIN32 AND NOT CMAKE_VERSION VERSION_LESS 3.22)
		set_property(TEST ${name} APPEND PROPERTY
			ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
	endif()
endfunction()

# eleza_add_test(<name> <source> [LIBS libs...] [GUI])
#   GUI tests run with the offscreen platform plugin.
function(eleza_add_test name source)
	cmake_parse_arguments(ARG "GUI" "" "LIBS" ${ARGN})
	add_executable(${name} ${source})
	target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Test ${ARG_LIBS})
	eleza_target_defaults(${name})
	add_test(NAME ${name} COMMAND ${name})
	if (ARG_GUI)
		set_tests_properties(${name} PROPERTIES
			ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
	endif()
	eleza_test_runtime_path(${name})
endfunction()

eleza_add_test(test_style_codegen ui/test_style_codegen.cpp LIBS codegen_style_lib)
eleza_add_test(test_lang_codegen ui/test_lang_codegen.cpp LIBS codegen_lang_lib)
eleza_add_test(test_lang_format ui/test_lang_format.cpp LIBS ElezaLang)
eleza_add_test(test_ui_text ui/test_ui_text.cpp LIBS ElezaUi Qt6::Gui GUI)
eleza_add_test(test_ui_style ui/test_ui_style.cpp LIBS ElezaUi Qt6::Gui GUI)
# Telegram's emoji: the pack reader against the real pack, the parser, the tables.
eleza_add_test(test_emoji ui/test_emoji.cpp LIBS ElezaUi Qt6::Gui GUI)
target_compile_definitions(test_emoji PRIVATE
	ELEZA_EMOJI_PACK="${ELEZA_RESOURCES_DIR}/emoji/emoji.pack")

# Parts of the executable that have no widget dependencies are compiled straight
# into their tests.
set(_eleza_app_src "${CMAKE_CURRENT_SOURCE_DIR}/../Eleza/SourceFiles")
eleza_add_test(test_routes app/test_routes.cpp)
target_sources(test_routes PRIVATE "${_eleza_app_src}/window/window_routes.cpp")
target_include_directories(test_routes PRIVATE "${_eleza_app_src}")
# What a link in somebody's text may open (support/links_parse.cpp has no widgets).
eleza_add_test(test_links app/test_links.cpp LIBS ElezaUi Qt6::Gui)
target_sources(test_links PRIVATE
	"${_eleza_app_src}/support/links_parse.cpp"
	"${_eleza_app_src}/window/window_routes.cpp")
target_include_directories(test_links PRIVATE "${_eleza_app_src}")
# The arithmetic of the custom window frame (hit-test, client rectangle, units).
eleza_add_test(test_title_hit_test app/test_title_hit_test.cpp)
target_sources(test_title_hit_test PRIVATE
	"${_eleza_app_src}/window/window_title_hit_test.cpp")
target_include_directories(test_title_hit_test PRIVATE "${_eleza_app_src}")
# The media screens: month headings and the grid of the shared media page.
eleza_add_test(test_media_logic app/test_media_logic.cpp LIBS ElezaLang)
target_sources(test_media_logic PRIVATE "${_eleza_app_src}/media/media_shared_logic.cpp")
target_include_directories(test_media_logic PRIVATE "${_eleza_app_src}")
# The order of the player's songs (newest first, ⏭ round, repeat, shuffle).
eleza_add_test(test_media_playlist app/test_media_playlist.cpp)
target_sources(test_media_playlist PRIVATE "${_eleza_app_src}/media/media_playlist.cpp")
target_include_directories(test_media_playlist PRIVATE "${_eleza_app_src}")
eleza_add_test(test_eli_logic app/test_eli_logic.cpp)
target_sources(test_eli_logic PRIVATE "${_eleza_app_src}/eli/eli_logic.cpp")
target_include_directories(test_eli_logic PRIVATE "${_eleza_app_src}")
if (TARGET ElezaCore)
	eleza_add_test(test_error_text app/test_error_text.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_error_text PRIVATE "${_eleza_app_src}/support/error_text.cpp")
	target_include_directories(test_error_text PRIVATE "${_eleza_app_src}")

	eleza_add_test(test_dialogs_logic app/test_dialogs_logic.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_dialogs_logic PRIVATE
		"${_eleza_app_src}/dialogs/dialogs_logic.cpp"
		"${_eleza_app_src}/support/chat_text.cpp")
	target_include_directories(test_dialogs_logic PRIVATE "${_eleza_app_src}")

	eleza_add_test(test_settings_logic app/test_settings_logic.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_settings_logic PRIVATE
		"${_eleza_app_src}/settings/settings_logic.cpp")
	target_include_directories(test_settings_logic PRIVATE "${_eleza_app_src}")
	# The chat screen's widget-free rules (history/history_*logic.cpp).
	eleza_add_test(test_history_logic app/test_history_logic.cpp LIBS ElezaCore ElezaUi)
	target_sources(test_history_logic PRIVATE "${_eleza_app_src}/history/history_logic.cpp")
	target_include_directories(test_history_logic PRIVATE "${_eleza_app_src}")
	# The emoji panel's data: categories, search, recents, tones.
	eleza_add_test(test_history_emoji app/test_history_emoji.cpp LIBS ElezaCore ElezaUi ElezaLang)
	target_sources(test_history_emoji PRIVATE
		"${_eleza_app_src}/history/history_emoji_data.cpp"
		"${_eleza_app_src}/history/history_logic.cpp")
	target_include_directories(test_history_emoji PRIVATE "${_eleza_app_src}")
	eleza_add_test(test_history_compose app/test_history_compose.cpp LIBS ElezaCore ElezaUi)
	target_sources(test_history_compose PRIVATE "${_eleza_app_src}/history/history_compose_logic.cpp")
	target_include_directories(test_history_compose PRIVATE "${_eleza_app_src}")
	# Chat albums: the mosaic, grouping a timeline, the send plan.
	eleza_add_test(test_history_album app/test_history_album.cpp LIBS ElezaCore)
	target_sources(test_history_album PRIVATE "${_eleza_app_src}/history/history_album.cpp")
	target_include_directories(test_history_album PRIVATE "${_eleza_app_src}")
	target_compile_definitions(test_history_album PRIVATE
		ELEZA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/testdata")
	# Bots in a chat: inline queries, commands, «شروع», inline keyboard buttons.
	eleza_add_test(test_history_bots app/test_history_bots.cpp LIBS ElezaCore)
	target_sources(test_history_bots PRIVATE "${_eleza_app_src}/history/history_bot_logic.cpp")
	target_include_directories(test_history_bots PRIVATE "${_eleza_app_src}")
endif()


# ---- conversation / search (widget-free logic of the two modules)
eleza_add_test(test_conversation_time app/test_conversation_time.cpp LIBS ElezaLang)
target_sources(test_conversation_time PRIVATE
	"${_eleza_app_src}/conversation/conversation_time.cpp")
target_include_directories(test_conversation_time PRIVATE "${_eleza_app_src}")
if (TARGET ElezaCore)
	eleza_add_test(test_conversation_radar app/test_conversation_radar.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_conversation_radar PRIVATE
		"${_eleza_app_src}/conversation/conversation_radar_model.cpp"
		"${_eleza_app_src}/conversation/conversation_radar_model.h")
	target_include_directories(test_conversation_radar PRIVATE
		"${_eleza_app_src}"
		"${CMAKE_CURRENT_SOURCE_DIR}/core")
	target_compile_definitions(test_conversation_radar PRIVATE
		ELEZA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/testdata")
	set_tests_properties(test_conversation_radar PROPERTIES TIMEOUT 120)

	eleza_add_test(test_search_logic app/test_search_logic.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_search_logic PRIVATE
		"${_eleza_app_src}/search/search_recent.cpp"
		"${_eleza_app_src}/search/search_debounce.cpp"
		"${_eleza_app_src}/search/search_debounce.h"
		"${_eleza_app_src}/search/search_people.cpp")
	target_include_directories(test_search_logic PRIVATE "${_eleza_app_src}")
	# profile: relationship -> buttons, handle check, bio, birthday ranges, QR.
	eleza_add_test(test_profile_logic app/test_profile_logic.cpp
		LIBS ElezaCore ElezaLang ElezaUi ElezaBase Qt6::Gui GUI)
	target_sources(test_profile_logic PRIVATE
		"${_eleza_app_src}/profile/profile_logic.cpp"
		"${_eleza_app_src}/profile/profile_qr.cpp")
	target_include_directories(test_profile_logic PRIVATE "${_eleza_app_src}")
endif()

# ---- in-app updates
# The install step (lib/install, no Qt): the swap, its rollback and recovery.
eleza_add_test(test_install_swap app/test_install_swap.cpp LIBS ElezaInstall)
set_tests_properties(test_install_swap PROPERTIES TIMEOUT 300)

# The real Updater.exe on a small fake install directory (Windows only): it
# waits for "the app", swaps the files and starts the new one again.
if (WIN32 AND TARGET Updater)
	foreach(_probe old new)
		add_executable(updater_probe_${_probe} WIN32 tools/updater_probe.cpp)
		set_target_properties(updater_probe_${_probe} PROPERTIES
			AUTOMOC OFF
			CXX_STANDARD 20
			CXX_STANDARD_REQUIRED ON
			MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_definitions(updater_probe_${_probe} PRIVATE
			UNICODE
			_UNICODE
			PROBE_NAME="${_probe}")
		target_link_libraries(updater_probe_${_probe} PRIVATE shell32)
		if (MINGW)
			target_link_options(updater_probe_${_probe} PRIVATE -municode -static)
		endif()
	endforeach()
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if (Python3_Interpreter_FOUND)
		add_test(NAME test_updater_smoke COMMAND
			"${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/tools/updater-smoke.py"
			--updater "$<TARGET_FILE:Updater>"
			--probe-old "$<TARGET_FILE:updater_probe_old>"
			--probe-new "$<TARGET_FILE:updater_probe_new>")
		set_tests_properties(test_updater_smoke PROPERTIES TIMEOUT 300)
	endif()
endif()

# ---- voice calls
if (ELEZA_ENABLE_CALLS)
	find_package(Qt6 6.4 REQUIRED COMPONENTS Multimedia)
	set(_eleza_calls_media_sources
		"${_eleza_app_src}/calls/calls_audio_io.cpp"
		"${_eleza_app_src}/calls/calls_audio_io.h"
		"${_eleza_app_src}/calls/calls_ffi.cpp"
		"${_eleza_app_src}/calls/calls_media.cpp"
		"${_eleza_app_src}/calls/calls_media.h"
		"${_eleza_app_src}/calls/calls_media_livekit.cpp"
		"${_eleza_app_src}/calls/calls_video.cpp"
		"${_eleza_app_src}/calls/calls_video.h")
	# A room participant on the command line: the media layer alone
	# (tests/tools/calls-e2e.sh, or by hand against a real SFU).
	add_executable(calls_media_probe tools/calls_media_probe.cpp
		${_eleza_calls_media_sources})
	target_include_directories(calls_media_probe PRIVATE "${_eleza_app_src}")
	target_link_libraries(calls_media_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia)
	target_compile_definitions(calls_media_probe PRIVATE
		ELEZA_VERSION_STRING="${PROJECT_VERSION}")
	eleza_target_defaults(calls_media_probe)
	eleza_livekit_deploy(calls_media_probe)
	# The pinned library loads on this platform and speaks the interface
	# calls_media_livekit.cpp was written against.
	add_test(NAME test_calls_media_library COMMAND calls_media_probe --self-check)
	set_tests_properties(test_calls_media_library PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 60)
	eleza_test_runtime_path(test_calls_media_library)

	# The wire codec of the library's messages (a header).
	eleza_add_test(test_calls_ffi_proto app/test_calls_ffi_proto.cpp)
	target_include_directories(test_calls_ffi_proto PRIVATE "${_eleza_app_src}")
	# Sample formats, the resampler, the playout queue, levels, the join
	# payload and the quality estimate: no device, no library.
	eleza_add_test(test_calls_audio app/test_calls_audio.cpp LIBS Qt6::Multimedia)
	target_sources(test_calls_audio PRIVATE
		"${_eleza_app_src}/calls/calls_audio_io.cpp"
		"${_eleza_app_src}/calls/calls_audio_io.h"
		"${_eleza_app_src}/calls/calls_ffi.cpp"
		"${_eleza_app_src}/calls/calls_media.cpp"
		"${_eleza_app_src}/calls/calls_media.h"
		"${_eleza_app_src}/calls/calls_video.cpp"
		"${_eleza_app_src}/calls/calls_video.h")
	target_include_directories(test_calls_audio PRIVATE "${_eleza_app_src}")
	target_compile_definitions(test_calls_audio PRIVATE
		ELEZA_VERSION_STRING="${PROJECT_VERSION}")

	if (TARGET ElezaCore)
		# The call state machine and the "which call" rules against a fake
		# server and a fake media (no library, no audio).
		eleza_add_test(test_calls_call app/test_calls_call.cpp LIBS ElezaCore)
		target_sources(test_calls_call PRIVATE
			"${_eleza_app_src}/calls/calls_call.cpp"
			"${_eleza_app_src}/calls/calls_call.h"
			"${_eleza_app_src}/calls/calls_instance.cpp"
			"${_eleza_app_src}/calls/calls_instance.h"
			"${_eleza_app_src}/calls/calls_ffi.cpp"
			"${_eleza_app_src}/calls/calls_media.cpp"
			"${_eleza_app_src}/calls/calls_media.h")
		target_include_directories(test_calls_call PRIVATE "${_eleza_app_src}")
		target_compile_definitions(test_calls_call PRIVATE
			ELEZA_VERSION_STRING="${PROJECT_VERSION}")
		set_tests_properties(test_calls_call PROPERTIES TIMEOUT 120)
	endif()
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
	add_subdirectory(core)
endif()
