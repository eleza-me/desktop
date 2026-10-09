add_library(codegen_style_lib STATIC
	style_parser.cpp
	style_parser.h
	style_generator.cpp
	style_generator.h
)
target_include_directories(codegen_style_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(codegen_style_lib PUBLIC Qt6::Core)
set_target_properties(codegen_style_lib PROPERTIES AUTOMOC OFF)
eleza_target_defaults(codegen_style_lib)

add_executable(codegen_style main.cpp)
target_link_libraries(codegen_style PRIVATE codegen_style_lib)
set_target_properties(codegen_style PROPERTIES AUTOMOC OFF)
eleza_target_defaults(codegen_style)
