# The Eleza executable: core/ (launcher, application, settings), window/ (main window,
# session layout, navigation), gallery/ and every feature folder that has sources.
#
# Feature modules only add files under SourceFiles/<module>/ and one registration call
# in SourceFiles/core/core_modules.cpp; this file picks the sources up by itself.

include("${CMAKE_CURRENT_LIST_DIR}/lang.cmake")
# The non-GUI core (crypto, api, rt, storage, data, main): the ElezaCore library.
include("${CMAKE_CURRENT_LIST_DIR}/core.cmake")

find_package(Qt6 6.4 REQUIRED COMPONENTS Multimedia)

set(eleza_gui_modules
	core
	window
	intro
	dialogs
	history
	conversation
	eli
	search
	profile
	settings
	boxes
	media
	support
)
if (ELEZA_DEBUG_TOOLS)
	# The widget gallery (--gallery) is a development tool.
	list(APPEND eleza_gui_modules gallery)
endif()
set(eleza_sources)
set(eleza_styles)
foreach(module ${eleza_gui_modules})
	file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles/${module}/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles/${module}/*.h")
	file(GLOB_RECURSE module_styles CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles/${module}/*.style")
	list(APPEND eleza_sources ${module_sources})
	list(APPEND eleza_styles ${module_styles})
endforeach()
file(GLOB eleza_resource_styles CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Resources/styles/*.style")
list(APPEND eleza_styles ${eleza_resource_styles})

add_executable(Eleza WIN32 ${eleza_sources})

target_include_directories(Eleza PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles")
target_link_libraries(Eleza PRIVATE
	ElezaUi
	ElezaLang
	ElezaCore
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
	Qt6::Network
	Qt6::Multimedia)
eleza_target_defaults(Eleza)
target_compile_definitions(Eleza PRIVATE
	ELEZA_VERSION_STRING="${PROJECT_VERSION}")

if (eleza_styles)
	eleza_generate_styles(Eleza
		${eleza_styles}
		INCLUDE_DIRS
			"${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles"
			"${CMAKE_CURRENT_SOURCE_DIR}/Resources/styles")
endif()

eleza_add_resources(Eleza
	"${CMAKE_CURRENT_SOURCE_DIR}/Resources/qrc/eleza.qrc"
	DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/Resources/art/icon512.png"
		"${CMAKE_CURRENT_SOURCE_DIR}/Resources/art/icon256.png"
		"${CMAKE_CURRENT_SOURCE_DIR}/Resources/art/icon64.png"
		"${CMAKE_CURRENT_SOURCE_DIR}/Resources/sounds/notify.wav")

if (WIN32)
	set(ELEZA_RC_VERSION_COMMA
		"${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/Resources/winrc/Eleza.rc.in"
		"${CMAKE_CURRENT_BINARY_DIR}/Eleza.rc"
		@ONLY)
	target_sources(Eleza PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Eleza.rc")
	set_target_properties(Eleza PROPERTIES
		VS_DPI_AWARE "PerMonitor"
		OUTPUT_NAME "Eleza")
else()
	set_target_properties(Eleza PROPERTIES OUTPUT_NAME "Eleza")
endif()

# ---- install
include(GNUInstallDirs)
install(TARGETS Eleza RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (UNIX AND NOT APPLE)
	install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Resources/linux/me.eleza.desktop.desktop"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
	foreach(size 16 32 48 64 128 256 512)
		install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Resources/art/icon${size}.png"
			DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
			RENAME "me.eleza.desktop.png")
	endforeach()
endif()
