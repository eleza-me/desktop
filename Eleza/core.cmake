# ElezaCore: everything under SourceFiles that has no widgets (crypto, storage, api, rt, data,
# main). Included by Eleza/CMakeLists.txt (`include(core.cmake OPTIONAL)`) and by the standalone
# tests/core project, so every path is relative to this file.
#
# Depends on ElezaBase (lib/base), QtCore / QtGui (QImage only) / QtNetwork / QtWebSockets /
# QtSql (SQLite) and OpenSSL 3. Never on QtWidgets or lib/ui.

if (TARGET ElezaCore)
	return()
endif()

set(_eleza_core_root "${CMAKE_CURRENT_LIST_DIR}")
set(_eleza_core_src "${_eleza_core_root}/SourceFiles")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network WebSockets Sql)
find_package(OpenSSL 3 REQUIRED)

if (NOT TARGET ElezaBase)
	add_subdirectory("${_eleza_core_root}/../lib/base" "${CMAKE_BINARY_DIR}/lib_base")
endif()

set(_eleza_core_files
	api/api_auth.cpp
	api/api_auth.h
	api/api_client.cpp
	api/api_client.h
	api/api_client_config.cpp
	api/api_client_config.h
	api/api_error.cpp
	api/api_error.h
	crypto/crypto_device_key.cpp
	crypto/crypto_device_key.h
	crypto/crypto_primitives.cpp
	crypto/crypto_primitives.h
	data/data_blurhash.cpp
	data/data_blurhash.h
	data/data_chats.cpp
	data/data_chats.h
	data/data_conversations.cpp
	data/data_conversations.h
	data/data_feed.cpp
	data/data_feed.h
	data/data_graph.cpp
	data/data_graph.h
	data/data_json.cpp
	data/data_json.h
	data/data_media.cpp
	data/data_media.h
	data/data_messages.cpp
	data/data_messages.h
	data/data_notifications.cpp
	data/data_notifications.h
	data/data_outbox.cpp
	data/data_outbox.h
	data/data_session.cpp
	data/data_session.h
	data/data_social_types.cpp
	data/data_social_types.h
	data/data_stickers.cpp
	data/data_stickers.h
	data/data_sync.cpp
	data/data_sync.h
	data/data_transport.cpp
	data/data_transport.h
	data/data_types.cpp
	data/data_types.h
	data/data_upload.cpp
	data/data_upload.h
	data/data_users.cpp
	data/data_users.h
	main/main_account.cpp
	main/main_account.h
	main/main_session.cpp
	main/main_session.h
	rt/rt_client.cpp
	rt/rt_client.h
	rt/rt_frames.cpp
	rt/rt_frames.h
	rt/rt_keys.cpp
	rt/rt_keys.h
	rt/rt_methods.cpp
	rt/rt_methods.h
	rt/rt_noise.cpp
	rt/rt_noise.h
	rt/rt_proto.cpp
	rt/rt_proto.h
	rt/rt_schema.h
	rt/rt_schema_auto.cpp
	rt/rt_schema_auto.h
	rt/rt_session.cpp
	rt/rt_session.h
	storage/storage_database.cpp
	storage/storage_database.h
	storage/storage_key_value.cpp
	storage/storage_key_value.h
	storage/storage_secure_blob.cpp
	storage/storage_secure_blob.h
)
set(_eleza_core_sources)
foreach(_file ${_eleza_core_files})
	list(APPEND _eleza_core_sources "${_eleza_core_src}/${_file}")
endforeach()

add_library(ElezaCore STATIC ${_eleza_core_sources})
set_target_properties(ElezaCore PROPERTIES
	AUTOMOC ON
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)
target_include_directories(ElezaCore PUBLIC "${_eleza_core_src}")
target_link_libraries(ElezaCore
	PUBLIC
		ElezaBase
		Qt6::Core
		Qt6::Gui
		Qt6::Network
		Qt6::WebSockets
		Qt6::Sql
	PRIVATE
		OpenSSL::Crypto)

if (NOT DEFINED ELEZA_CORE_VERSION)
	if (PROJECT_VERSION)
		set(ELEZA_CORE_VERSION "${PROJECT_VERSION}")
	else()
		set(ELEZA_CORE_VERSION "0.1.0")
	endif()
endif()
target_compile_definitions(ElezaCore PRIVATE ELEZA_CORE_VERSION="${ELEZA_CORE_VERSION}")

if (WIN32)
	# DPAPI (CryptProtectData) for Storage::SecureBlob.
	target_link_libraries(ElezaCore PRIVATE crypt32)
endif()
if (MSVC)
	target_compile_options(ElezaCore PRIVATE /W4 /utf-8 /bigobj)
else()
	target_compile_options(ElezaCore PRIVATE -Wall -Wextra)
endif()

# Regenerate the realtime schema tables after the .proto files change:
#   cmake --build <dir> --target eleza_gen_rtschema
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND AND NOT TARGET eleza_gen_rtschema)
	add_custom_target(eleza_gen_rtschema
		COMMAND "${Python3_EXECUTABLE}" "${_eleza_core_root}/../codegen/scripts/gen_rtschema.py"
		COMMENT "Generating rt_schema_auto from Resources/proto"
		VERBATIM)
endif()
