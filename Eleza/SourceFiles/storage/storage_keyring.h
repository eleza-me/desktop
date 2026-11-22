// The desktop's secret store, where the keys that protect the files of
// storage_secure_blob.h live on Linux: the Secret Service (GNOME Keyring,
// KWallet, KeePassXC…) over libsecret.
//
// libsecret is loaded at run time (libsecret-1.so.0), never linked: the app
// starts and works without it, and without a running service. Then there is
// no keyring (Default() is null) and the blobs stay owner-only files, as
// before; one log line says so. ELEZA_SECRET_SERVICE=0 turns it off (the
// tests do, so that they never touch the keyring of whoever runs them).
//
// Windows has DPAPI for this and no keyring here. Every call blocks (D-Bus):
// a handful at start and sign-in, on the main thread.
#pragma once

#include "base/basic_types.h"

namespace Storage {

class Keyring {
public:
	enum class Lookup {
		Found,
		Missing,     // the service answered: there is no such secret
		Unavailable, // no answer (locked and not unlocked, gone, an error)
	};

	virtual ~Keyring() = default;

	// `secret` is printable ASCII; `id` names it (one secret per id).
	[[nodiscard]] virtual bool store(
		const QString &id,
		const QString &label,
		const QByteArray &secret) = 0;
	[[nodiscard]] virtual Lookup lookup(const QString &id, QByteArray &secret) = 0;
	virtual void remove(const QString &id) = 0;

	// The Secret Service when libsecret is there and a service answers; null
	// otherwise (and always on Windows). Probed once per process.
	[[nodiscard]] static Keyring *Default();
	// Tests: what Default() returns from now on (null: no keyring).
	static void SetDefaultForTests(Keyring *keyring);

};

} // namespace Storage
