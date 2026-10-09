// A small secret kept on disk (tokens, the device private key).
//
// Windows: the bytes are sealed with DPAPI (CryptProtectData, bound to the
// user's logon) before they are written. Linux and others: a file readable
// by the owner only (0600) inside a 0700 directory. Same API everywhere.
#pragma once

#include "base/basic_types.h"

namespace Storage {

class SecureBlob {
public:
	explicit SecureBlob(const QString &path);

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
	QString _path;

};

} // namespace Storage
