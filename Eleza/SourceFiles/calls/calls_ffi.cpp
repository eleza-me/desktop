#include "calls/calls_ffi.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QMutex>

#include <cstddef>
#include <cstdint>

namespace Calls::Ffi {
namespace {

// The C interface of the library (livekit_ffi.h of the pinned release).
using EventCallback = void (*)(const uint8_t *data, size_t size);
using InitializeFn = void (*)(
	EventCallback callback,
	bool captureLogs,
	const char *sdk,
	const char *sdkVersion);
using RequestFn = uint64_t (*)(
	const uint8_t *data,
	size_t size,
	const uint8_t **responseData,
	size_t *responseSize);
using DropHandleFn = bool (*)(uint64_t handle);
using DisposeFn = void (*)();

struct State {
	QMutex mutex;
	Library *instance = nullptr;
	QString error;
	bool tried = false;
	std::shared_ptr<std::function<void(QByteArray)>> handler;
};

[[nodiscard]] State &Shared() {
	static auto result = State();
	return result;
}

void HandleEvent(const uint8_t *data, size_t size) {
	auto handler = std::shared_ptr<std::function<void(QByteArray)>>();
	{
		auto &state = Shared();
		const auto lock = QMutexLocker(&state.mutex);
		handler = state.handler;
	}
	if (handler && *handler && data && size > 0) {
		(*handler)(QByteArray(reinterpret_cast<const char*>(data), qsizetype(size)));
	}
}

} // namespace

struct Library::Functions {
	QLibrary library;
	InitializeFn initialize = nullptr;
	RequestFn request = nullptr;
	DropHandleFn dropHandle = nullptr;
	DisposeFn dispose = nullptr;
	bool disposed = false;
};

QString Library::FileName() {
#ifdef Q_OS_WIN
	return QStringLiteral("livekit_ffi.dll");
#elif defined Q_OS_MACOS // Q_OS_WIN
	return QStringLiteral("liblivekit_ffi.dylib");
#else // Q_OS_WIN || Q_OS_MACOS
	return QStringLiteral("liblivekit_ffi.so");
#endif // Q_OS_WIN || Q_OS_MACOS
}

QStringList Library::Candidates() {
	auto result = QStringList();
#ifdef ELEZA_DEBUG_TOOLS
	const auto custom = qEnvironmentVariable("ELEZA_LIVEKIT_FFI");
	if (!custom.isEmpty()) {
		result.push_back(QFileInfo(custom).absoluteFilePath());
	}
#endif // ELEZA_DEBUG_TOOLS
	// Absolute paths only: the name alone would be searched for in places
	// the app does not control.
	const auto directory = QDir(QCoreApplication::applicationDirPath());
	result.push_back(directory.absoluteFilePath(FileName()));
#ifndef Q_OS_WIN
	result.push_back(QDir::cleanPath(
		directory.absoluteFilePath(QStringLiteral("../lib/eleza/") + FileName())));
#endif // !Q_OS_WIN
	return result;
}

bool Library::Present() {
	for (const auto &path : Candidates()) {
		if (QFileInfo(path).isFile()) {
			return true;
		}
	}
	return false;
}

QString Library::LoadError() {
	auto &state = Shared();
	const auto lock = QMutexLocker(&state.mutex);
	return state.error;
}

Library *Library::Instance() {
	auto &state = Shared();
	const auto lock = QMutexLocker(&state.mutex);
	if (state.tried) {
		return state.instance;
	}
	state.tried = true;
	auto functions = std::make_unique<Functions>();
	auto path = QString();
	for (const auto &candidate : Candidates()) {
		if (QFileInfo(candidate).isFile()) {
			path = candidate;
			break;
		}
	}
	if (path.isEmpty()) {
		state.error = QStringLiteral("%1 was not found").arg(FileName());
		return nullptr;
	}
	functions->library.setFileName(path);
	if (!functions->library.load()) {
		state.error = functions->library.errorString();
		return nullptr;
	}
	functions->initialize = reinterpret_cast<InitializeFn>(
		functions->library.resolve("livekit_ffi_initialize"));
	functions->request = reinterpret_cast<RequestFn>(
		functions->library.resolve("livekit_ffi_request"));
	functions->dropHandle = reinterpret_cast<DropHandleFn>(
		functions->library.resolve("livekit_ffi_drop_handle"));
	functions->dispose = reinterpret_cast<DisposeFn>(
		functions->library.resolve("livekit_ffi_dispose"));
	if (!functions->initialize
		|| !functions->request
		|| !functions->dropHandle
		|| !functions->dispose) {
		state.error = QStringLiteral("%1 has another interface").arg(FileName());
		// The library started threads of its own when it was loaded: it
		// stays in the process.
		[[maybe_unused]] const auto leaked = functions.release();
		return nullptr;
	}
	// The second argument: the library keeps its log lines to itself (they
	// would arrive as events otherwise).
	functions->initialize(
		&HandleEvent,
		false,
		"eleza-desktop",
		ELEZA_VERSION_STRING);
	// Never unloaded and never destroyed: the library's threads live until
	// the process ends.
	state.instance = new Library();
	state.instance->_functions = std::move(functions);
	return state.instance;
}

void Library::setEventHandler(std::function<void(QByteArray)> handler) {
	auto &state = Shared();
	const auto lock = QMutexLocker(&state.mutex);
	state.handler = handler
		? std::make_shared<std::function<void(QByteArray)>>(std::move(handler))
		: nullptr;
}

QByteArray Library::request(int field, const Writer &message) {
	if (_functions->disposed) {
		return QByteArray();
	}
	auto wrapped = Writer();
	wrapped.message(field, message);
	const auto &bytes = wrapped.data();
	const uint8_t *data = nullptr;
	auto size = size_t();
	const auto handle = _functions->request(
		reinterpret_cast<const uint8_t*>(bytes.constData()),
		size_t(bytes.size()),
		&data,
		&size);
	if (!handle) {
		return QByteArray();
	}
	// The answer belongs to the handle: copy it, then give the handle back.
	// (An FfiResponse always names what it answers, so it is never empty.)
	const auto result = (data && size > 0)
		? QByteArray(reinterpret_cast<const char*>(data), qsizetype(size))
		: QByteArray();
	_functions->dropHandle(handle);
	return result;
}

void Library::dropHandle(quint64 id) {
	if (id && !_functions->disposed) {
		_functions->dropHandle(id);
	}
}

void Library::dispose() {
	if (!_functions->disposed) {
		_functions->disposed = true;
		_functions->dispose();
	}
}

} // namespace Calls::Ffi
