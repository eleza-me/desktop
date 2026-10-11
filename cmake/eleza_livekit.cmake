# The media library of voice calls: LiveKit's `livekit_ffi` (the room client
# with its WebRTC stack behind a small C interface; Apache-2.0, see
# third_party/livekit/README.md for what it contains).
#
# Nothing is compiled here and nothing links against the library: the app loads
# it at run time (Eleza/SourceFiles/calls/calls_ffi.cpp) and starts without it.
# This module only fetches the prebuilt binary of the pinned release from the
# project's GitHub releases, checks its SHA-256 and says where the files are:
#
#   ELEZA_LIVEKIT_FFI_LIBRARY   the shared library (liblivekit_ffi.so / livekit_ffi.dll)
#   ELEZA_LIVEKIT_FFI_LICENSE   the notices of everything inside it (LICENSE.md)
#   eleza_livekit_deploy(<target>)   copies both next to the target's binary
#
# To update: change the version and the three hashes together (sha256sum of
# the release archives), read the protocol changes between the two tags
# (livekit-ffi/protocol/*.proto: calls_media_livekit.cpp names the fields it
# uses) and run tests/tools/calls-e2e.sh.
#
# Without network access: download the archive of the platform elsewhere and
# pass -DELEZA_LIVEKIT_FFI_ARCHIVE=<file> (the hash is checked all the same),
# or build without calls: -DELEZA_ENABLE_CALLS=OFF.

set(ELEZA_LIVEKIT_FFI_VERSION "0.12.48")
set(_eleza_livekit_base
	"https://github.com/livekit/rust-sdks/releases/download/rust-sdks%2Flivekit-ffi%40${ELEZA_LIVEKIT_FFI_VERSION}")
set(_eleza_livekit_sha256_linux_x86_64
	"50fedb2b9207dacb0aef025e12c8e98271bc130bc5e8ec2458684a7c4143b93f")
set(_eleza_livekit_sha256_linux_arm64
	"00cdf1ecfc44795e9cbf466c9c847394ed19b1437a2315e37aa56e25c13b71bb")
set(_eleza_livekit_sha256_windows_x86_64
	"9dc26ec4f5c06301e073f79a6e5a7806b1d87d329bd0b372b87e065c21cd6afa")

set(ELEZA_LIVEKIT_FFI_ARCHIVE "" CACHE FILEPATH
	"A downloaded ffi-<platform>.zip of livekit-ffi ${ELEZA_LIVEKIT_FFI_VERSION} to use instead of downloading it.")

set(ELEZA_LIVEKIT_FFI_LIBRARY "")
set(ELEZA_LIVEKIT_FFI_LICENSE "")

function(_eleza_livekit_fetch)
	string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)
	if (processor MATCHES "^(x86_64|amd64)$")
		set(arch "x86_64")
	elseif (processor MATCHES "^(aarch64|arm64)$")
		set(arch "arm64")
	else()
		set(arch "")
	endif()
	if (WIN32 AND arch STREQUAL "x86_64")
		set(platform "windows-x86_64")
		set(library "livekit_ffi.dll")
		set(sha256 "${_eleza_livekit_sha256_windows_x86_64}")
	elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND arch STREQUAL "x86_64")
		set(platform "linux-x86_64")
		set(library "liblivekit_ffi.so")
		set(sha256 "${_eleza_livekit_sha256_linux_x86_64}")
	elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND arch STREQUAL "arm64")
		set(platform "linux-arm64")
		set(library "liblivekit_ffi.so")
		set(sha256 "${_eleza_livekit_sha256_linux_arm64}")
	else()
		message(WARNING
			"Voice calls: no prebuilt media library for ${CMAKE_SYSTEM_NAME} "
			"${CMAKE_SYSTEM_PROCESSOR}. The app is built with the call screens, "
			"and reports calls as unavailable at run time. "
			"-DELEZA_ENABLE_CALLS=OFF builds without them.")
		return()
	endif()

	set(directory "${CMAKE_BINARY_DIR}/_deps/livekit-ffi-${ELEZA_LIVEKIT_FFI_VERSION}-${platform}")
	set(stamp "${directory}/.verified-${sha256}")
	if (NOT EXISTS "${stamp}" OR NOT EXISTS "${directory}/${library}")
		file(REMOVE_RECURSE "${directory}")
		file(MAKE_DIRECTORY "${directory}")
		set(archive "${directory}/ffi-${platform}.zip")
		if (ELEZA_LIVEKIT_FFI_ARCHIVE)
			if (NOT EXISTS "${ELEZA_LIVEKIT_FFI_ARCHIVE}")
				message(FATAL_ERROR
					"ELEZA_LIVEKIT_FFI_ARCHIVE does not exist: ${ELEZA_LIVEKIT_FFI_ARCHIVE}")
			endif()
			file(SHA256 "${ELEZA_LIVEKIT_FFI_ARCHIVE}" actual)
			if (NOT actual STREQUAL sha256)
				message(FATAL_ERROR
					"ELEZA_LIVEKIT_FFI_ARCHIVE is not ffi-${platform}.zip of livekit-ffi "
					"${ELEZA_LIVEKIT_FFI_VERSION}:\n  expected sha256 ${sha256}\n  got             ${actual}")
			endif()
			file(COPY_FILE "${ELEZA_LIVEKIT_FFI_ARCHIVE}" "${archive}")
		else()
			set(url "${_eleza_livekit_base}/ffi-${platform}.zip")
			message(STATUS "Voice calls: downloading ${url}")
			file(DOWNLOAD "${url}" "${archive}"
				EXPECTED_HASH "SHA256=${sha256}"
				TLS_VERIFY ON
				INACTIVITY_TIMEOUT 60
				STATUS status
				LOG log)
			list(GET status 0 code)
			if (NOT code EQUAL 0)
				list(GET status 1 text)
				file(REMOVE "${archive}")
				message(FATAL_ERROR
					"Voice calls: the media library could not be downloaded (${text}).\n"
					"  ${url}\n"
					"Download it by hand and pass -DELEZA_LIVEKIT_FFI_ARCHIVE=<file>, "
					"or configure with -DELEZA_ENABLE_CALLS=OFF to build without calls.")
			endif()
		endif()
		file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${directory}")
		file(REMOVE "${archive}")
		if (NOT EXISTS "${directory}/${library}")
			message(FATAL_ERROR
				"Voice calls: ffi-${platform}.zip has no ${library} in it.")
		endif()
		file(WRITE "${stamp}" "")
	endif()
	file(SIZE "${directory}/${library}" size)
	math(EXPR megabytes "${size} / 1048576")
	message(STATUS
		"Voice calls: livekit-ffi ${ELEZA_LIVEKIT_FFI_VERSION} (${platform}, ${megabytes} MB)")
	set(ELEZA_LIVEKIT_FFI_LIBRARY "${directory}/${library}" PARENT_SCOPE)
	if (EXISTS "${directory}/LICENSE.md")
		set(ELEZA_LIVEKIT_FFI_LICENSE "${directory}/LICENSE.md" PARENT_SCOPE)
	endif()
endfunction()

if (ELEZA_ENABLE_CALLS)
	_eleza_livekit_fetch()
endif()

# eleza_livekit_deploy(<target>)
#   The library (and its notices, as livekit_ffi-LICENSE.md) next to the built
#   binary of <target>, so that it runs from the build directory.
function(eleza_livekit_deploy target)
	if (NOT ELEZA_LIVEKIT_FFI_LIBRARY)
		return()
	endif()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${ELEZA_LIVEKIT_FFI_LIBRARY}"
			"$<TARGET_FILE_DIR:${target}>"
		VERBATIM)
	if (ELEZA_LIVEKIT_FFI_LICENSE)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${ELEZA_LIVEKIT_FFI_LICENSE}"
				"$<TARGET_FILE_DIR:${target}>/livekit_ffi-LICENSE.md"
			VERBATIM)
	endif()
endfunction()
