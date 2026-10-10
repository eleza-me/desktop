# ElezaUpdater: the widget-free part of the in-app updates (SourceFiles/updater):
# the signed manifest, the package unpacker, the checker (check -> download ->
# verify -> unpack) and the glue around the restart. Linked by the Eleza
# executable and by tests/core/test_updater.
#
# Depends on ElezaCore (crypto: Ed25519, SHA-256), ElezaInstall (the file swap,
# no Qt), ElezaMiniz (third_party) and QtNetwork.

if (TARGET ElezaUpdater)
	return()
endif()

set(_eleza_updater_src "${CMAKE_CURRENT_LIST_DIR}/SourceFiles")

add_library(ElezaUpdater STATIC
	"${_eleza_updater_src}/updater/updater_archive.cpp"
	"${_eleza_updater_src}/updater/updater_archive.h"
	"${_eleza_updater_src}/updater/updater_checker.cpp"
	"${_eleza_updater_src}/updater/updater_checker.h"
	"${_eleza_updater_src}/updater/updater_install.cpp"
	"${_eleza_updater_src}/updater/updater_install.h"
	"${_eleza_updater_src}/updater/updater_keys.h"
	"${_eleza_updater_src}/updater/updater_manifest.cpp"
	"${_eleza_updater_src}/updater/updater_manifest.h"
)
set_target_properties(ElezaUpdater PROPERTIES
	AUTOMOC ON
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)
target_include_directories(ElezaUpdater PUBLIC "${_eleza_updater_src}")
target_link_libraries(ElezaUpdater
	PUBLIC
		ElezaCore
		ElezaBase
		Qt6::Core
		Qt6::Network
	PRIVATE
		ElezaInstall
		ElezaMiniz)
eleza_target_defaults(ElezaUpdater)
