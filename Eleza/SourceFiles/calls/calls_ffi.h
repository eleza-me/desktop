// The media library (LiveKit's `livekit_ffi`: the room client with its WebRTC
// stack behind a C interface of four functions that exchange protobuf
// messages), loaded at run time.
//
// Nothing links against it: the app starts without the file, and calls are
// then reported as unavailable (Calls::MediaProblem). The file is looked for
// next to the executable, then in ../lib/eleza (an installed package); a build
// with the debug tools also takes ELEZA_LIVEKIT_FFI=<file>.
//
//   if (const auto library = Calls::Ffi::Library::Instance()) {
//       library->setEventHandler([](QByteArray event) { /* any thread */ });
//       const auto response = library->request(message);   // FfiRequest -> FfiResponse
//   }
//
// request() is synchronous but never waits for the network: what takes time
// is answered with an id first and arrives later as an event.
#pragma once

#include "calls/calls_ffi_proto.h"

#include <QtCore/QStringList>

#include <functional>
#include <memory>

namespace Calls::Ffi {

class Library final {
public:
	// Loads and initializes the library on first use. Null when it cannot be
	// loaded: LoadError() says why (for the log, not for the user).
	[[nodiscard]] static Library *Instance();
	[[nodiscard]] static QString LoadError();
	// Whether the file is there (nothing is loaded).
	[[nodiscard]] static bool Present();
	[[nodiscard]] static QString FileName();
	[[nodiscard]] static QStringList Candidates();

	// Events arrive on threads of the library: the handler must only hand
	// the bytes over to its own thread. One handler at a time.
	void setEventHandler(std::function<void(QByteArray)> handler);

	// Sends an FfiRequest whose only field is `field` = `message` and returns
	// the FfiResponse. An empty answer: the library refused the request.
	[[nodiscard]] QByteArray request(int field, const Writer &message);
	void dropHandle(quint64 id);

	// Closes every room and stops the events (the app is exiting). Blocks
	// until the rooms are closed: call it from a worker thread.
	void dispose();

private:
	Library() = default;

	struct Functions;
	std::unique_ptr<Functions> _functions;

};

} // namespace Calls::Ffi
