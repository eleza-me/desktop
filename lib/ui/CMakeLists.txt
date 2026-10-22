# ElezaUi — the custom-painted widget library. Depends only on Qt and ElezaBase (QtCore only).
file(GLOB_RECURSE eleza_ui_sources CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/ui/*.h")
file(GLOB_RECURSE eleza_ui_styles CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/ui/*.style")
file(GLOB eleza_ui_icons CONFIGURE_DEPENDS "${ELEZA_RESOURCES_DIR}/icons/*.svg")
file(GLOB eleza_ui_fonts CONFIGURE_DEPENDS "${ELEZA_RESOURCES_DIR}/fonts/*.ttf")

add_library(ElezaUi STATIC ${eleza_ui_sources})
add_library(Eleza::Ui ALIAS ElezaUi)

target_include_directories(ElezaUi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ElezaUi PUBLIC ElezaBase Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
eleza_target_defaults(ElezaUi)

eleza_generate_palette(ElezaUi "${ELEZA_PALETTE_FILE}")
eleza_generate_styles(ElezaUi
	${eleza_ui_styles}
	INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}")

eleza_add_resources(ElezaUi
	"${ELEZA_RESOURCES_DIR}/qrc/fonts.qrc"
	"${ELEZA_RESOURCES_DIR}/qrc/icons.qrc"
	DEPENDS ${eleza_ui_icons} ${eleza_ui_fonts})
