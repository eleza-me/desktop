// A small secret kept on disk (tokens, the device private key, the key of
// the message cache).
//
// Windows: the bytes are sealed with DPAPI (CryptProtectData, bound to the
// user's logon) before they are written. Linux and others: encrypted
// (AES-256-GCM) with a key of the blob's own that is kept in the desktop's
// Secret Service (storage_keyring.h), so the file alone (a backup, another
// process reading the disk, a copied home) gives nothing away. Without a
// Secret Service, a file readable by the owner only (0600) inside a 0700
// directory, as before; such a file moves into the keyring at the first read
// once a service is there. Same API everywhere.
#pragma once

#include "base/basic_types.h"

namespace Storage {

class SecureBlob {
public:
	explicit SecureBlob(const QString &path);
	~SecureBlob();
	SecureBlob(const SecureBlob &) = delete;
	SecureBlob &operator=(const SecureBlob &) = delete;

	[[nodiscard]] QString path() const {
		return _path;
	}
	[[nodiscard]] bool exists() const;

	// nullopt when the blob is missing or cannot be unsealed (another user,
	// a copied profile): the caller starts from scratch.
	[[nodiscard]] std::optional<QByteArray> read() const;
	bool write(const QByteArray &bytes) const;
	void remove() const;

private:
	[[nodiscard]] QString keyringId() const;
	// The blob's key in the keyring, created when `create` and missing.
	[[nodiscard]] std::optional<QByteArray> keyringKey(bool create) const;
	[[nodiscard]] std::optional<QByteArray> writeToKeyring(const QByteArray &bytes) const;

	QString _path;
	mutable QByteArray _key; // keyringKey(), once known

};

} // namespace Storage
