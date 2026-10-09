# A CPack configuration for a .deb of Eleza Desktop, used without touching the
# project's CMake files: it packages what `cmake --install` of an existing
# build directory installs (the binary, the desktop entry, the icons).
#
#   ELEZA_BUILD_DIR=<build dir> cpack --config packaging/linux/cpack-deb.cmake -B <out dir>
#
# or simply packaging/linux/build-deb.sh, which configures and builds first.
# The build must have been configured with -DCMAKE_INSTALL_PREFIX=/usr.

if (NOT DEFINED ELEZA_BUILD_DIR)
	set(ELEZA_BUILD_DIR "$ENV{ELEZA_BUILD_DIR}")
endif()
if (NOT ELEZA_BUILD_DIR OR NOT EXISTS "${ELEZA_BUILD_DIR}/CMakeCache.txt")
	message(FATAL_ERROR
		"Set ELEZA_BUILD_DIR to a configured build directory of Eleza "
		"(got \"${ELEZA_BUILD_DIR}\").")
endif()
get_filename_component(ELEZA_BUILD_DIR "${ELEZA_BUILD_DIR}" ABSOLUTE)

# The version comes from the build itself.
file(STRINGS "${ELEZA_BUILD_DIR}/CMakeCache.txt" _eleza_version_line
	REGEX "^CMAKE_PROJECT_VERSION:")
string(REGEX REPLACE "^[^=]*=" "" ELEZA_VERSION "${_eleza_version_line}")
if (NOT ELEZA_VERSION)
	message(FATAL_ERROR "No CMAKE_PROJECT_VERSION in ${ELEZA_BUILD_DIR}/CMakeCache.txt")
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_CMAKE_GENERATOR "Ninja")
set(CPACK_INSTALL_CMAKE_PROJECTS "${ELEZA_BUILD_DIR};Eleza;ALL;/")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_SET_DESTDIR OFF)
set(CPACK_STRIP_FILES ON)

# What include(CPack) would have filled in from the project.
set(CPACK_SYSTEM_NAME "Linux")
set(CPACK_TOPLEVEL_TAG "Linux")
set(CPACK_PACKAGE_FILE_NAME "eleza-desktop-${ELEZA_VERSION}-Linux")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Eleza")

set(CPACK_PACKAGE_NAME "eleza-desktop")
set(CPACK_PACKAGE_VENDOR "eleza")
set(CPACK_PACKAGE_VERSION "${ELEZA_VERSION}")
set(CPACK_PACKAGE_CONTACT "eleza <support@eleza.me>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://eleza.me")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Eleza Desktop: messenger, anonymous conversations and the Eli feed")
set(CPACK_PACKAGE_DESCRIPTION
	"The native desktop client of Eleza: one-to-one messaging, anonymous talker / listener conversations and the Eli social feed.")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# Libraries come from dpkg-shlibdeps; plugins that are loaded at run time are
# not seen by it and are listed by hand: the SQLite driver (the local cache),
# the platform plugins, the WebP decoder (stickers are WebP).
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
	"libqt6sql6-sqlite, qt6-qpa-plugins, qt6-image-formats-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fonts-noto-color-emoji")
