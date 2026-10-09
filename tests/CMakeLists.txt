# Unit tests (QtTest, registered with CTest):  ctest --test-dir <build> --output-on-failure
#
#   tests/ui/    style codegen, lang codegen, lang formatting (Jalali, digits), Ui::Text
#   tests/core/  the non-GUI core (added when present)

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

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
	add_subdirectory(core)
endif()
