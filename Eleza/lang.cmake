# ElezaLang: the language runtime (fa / en strings generated from Resources/langs,
# number + Jalali date formatting, the countries list). QtCore only, no widgets, so the
# non-GUI core may link it too (target_link_libraries(ElezaCore PUBLIC ElezaLang)).

if (TARGET ElezaLang)
	return()
endif()

set(_eleza_lang_root "${CMAKE_CURRENT_LIST_DIR}")

add_library(ElezaLang STATIC
	"${_eleza_lang_root}/SourceFiles/lang/lang_countries.cpp"
	"${_eleza_lang_root}/SourceFiles/lang/lang_countries.h"
	"${_eleza_lang_root}/SourceFiles/lang/lang_format.cpp"
	"${_eleza_lang_root}/SourceFiles/lang/lang_format.h"
	"${_eleza_lang_root}/SourceFiles/lang/lang_instance.cpp"
	"${_eleza_lang_root}/SourceFiles/lang/lang_instance.h"
)
add_library(Eleza::Lang ALIAS ElezaLang)
target_include_directories(ElezaLang PUBLIC "${_eleza_lang_root}/SourceFiles")
if (NOT TARGET ElezaBase)
	add_subdirectory("${_eleza_lang_root}/../lib/base" "${CMAKE_BINARY_DIR}/lib_base")
endif()
target_link_libraries(ElezaLang PUBLIC ElezaBase Qt6::Core)
eleza_target_defaults(ElezaLang)
eleza_generate_lang(ElezaLang
	"${_eleza_lang_root}/Resources/langs/lang.strings"
	"${_eleza_lang_root}/Resources/langs/lang_en.strings")
