# Unit tests (QtTest, registered with CTest):  ctest --test-dir <build> --output-on-failure
#
#   tests/ui/    style codegen, lang codegen, lang formatting (Jalali, digits), Ui::Text
#   tests/core/  the non-GUI core (added when present)
#   tests/app/   widget-free parts of the application: routes, error texts

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Test)

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
endfunction()

eleza_add_test(test_style_codegen ui/test_style_codegen.cpp LIBS codegen_style_lib)
eleza_add_test(test_lang_codegen ui/test_lang_codegen.cpp LIBS codegen_lang_lib)
eleza_add_test(test_lang_format ui/test_lang_format.cpp LIBS ElezaLang)
eleza_add_test(test_ui_text ui/test_ui_text.cpp LIBS ElezaUi Qt6::Gui GUI)
eleza_add_test(test_ui_style ui/test_ui_style.cpp LIBS ElezaUi Qt6::Gui GUI)

# Parts of the executable that have no widget dependencies are compiled straight
# into their tests.
set(_eleza_app_src "${CMAKE_CURRENT_SOURCE_DIR}/../Eleza/SourceFiles")
eleza_add_test(test_routes app/test_routes.cpp)
target_sources(test_routes PRIVATE "${_eleza_app_src}/window/window_routes.cpp")
target_include_directories(test_routes PRIVATE "${_eleza_app_src}")
if (TARGET ElezaCore)
	eleza_add_test(test_error_text app/test_error_text.cpp LIBS ElezaCore ElezaLang)
	target_sources(test_error_text PRIVATE "${_eleza_app_src}/support/error_text.cpp")
	target_include_directories(test_error_text PRIVATE "${_eleza_app_src}")

	# The chat screen's widget-free rules (history/history_*logic.cpp).
	eleza_add_test(test_history_logic app/test_history_logic.cpp LIBS ElezaCore ElezaUi)
	target_sources(test_history_logic PRIVATE "${_eleza_app_src}/history/history_logic.cpp")
	target_include_directories(test_history_logic PRIVATE "${_eleza_app_src}")
	eleza_add_test(test_history_compose app/test_history_compose.cpp LIBS ElezaCore ElezaUi)
	target_sources(test_history_compose PRIVATE "${_eleza_app_src}/history/history_compose_logic.cpp")
	target_include_directories(test_history_compose PRIVATE "${_eleza_app_src}")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
	add_subdirectory(core)
endif()
