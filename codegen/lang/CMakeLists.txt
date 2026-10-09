add_library(codegen_lang_lib STATIC
	lang_parser.cpp
	lang_parser.h
	lang_generator.cpp
	lang_generator.h
)
target_include_directories(codegen_lang_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(codegen_lang_lib PUBLIC Qt6::Core)
set_target_properties(codegen_lang_lib PROPERTIES AUTOMOC OFF)
eleza_target_defaults(codegen_lang_lib)

add_executable(codegen_lang main.cpp)
target_link_libraries(codegen_lang PRIVATE codegen_lang_lib)
set_target_properties(codegen_lang PROPERTIES AUTOMOC OFF)
eleza_target_defaults(codegen_lang)
