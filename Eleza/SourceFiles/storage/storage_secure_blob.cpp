#include "storage/storage_secure_blob.h"

#include "base/bytes.h"
#include "base/platform_file.h"
#include "crypto/crypto_primitives.h"
#include "storage/storage_keyring.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif // !NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#endif // Q_OS_WIN

namespace Storage {
namespace {

// A marker so a plain file is never mistaken for a sealed one (and back).
// ELZB1: DPAPI on Windows, the bytes themselves elsewhere. ELZB2 (not on
// Windows): nonce || AES-256-GCM(bytes) under the blob's key in the keyring.
constexpr char kMagic[] = "ELZB1";
constexpr char kKeyringMagic[] = "ELZB2";
constexpr auto kMagicSize = int(sizeof(kMagic) - 1);
static_assert(sizeof(kKeyringMagic) == sizeof(kMagic));
constexpr auto kWrapNonceSize = 12;

#ifdef Q_OS_WIN

[[nodiscard]] std::optional<QByteArray> Seal(const QByteArray &bytes) {
	auto input = DATA_BLOB();
	input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()));
	input.cbData = DWORD(bytes.size());
	auto output = DATA_BLOB();
	if (!CryptProtectData(
			&input,
			L"Eleza",
			nullptr,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&output)) {
		return std::nullopt;
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(output.pbData),
		qsizetype(output.cbData));
	LocalFree(output.pbData);
	return result;
}

[[nodiscard]] std::optional<QByteArray> Unseal(const QByteArray &bytes) {
	auto input = DATA_BLOB();
	input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()));
	input.cbData = DWORD(bytes.size());
	auto output = DATA_BLOB();
	if (!CryptUnprotectData(
			&input,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&output)) {
		return std::nullopt;
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(output.pbData),
		qsizetype(output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	if (result.isNull()) {
		result = QByteArray("", 0);
	}
	return result;
}

#else // Q_OS_WIN

[[nodiscard]] std::optional<QByteArray> Seal(const QByteArray &bytes) {
	return bytes;
}

[[nodiscard]] std::optional<QByteArray> Unseal(const QByteArray &bytes) {
	return bytes.isNull() ? QByteArray("", 0) : bytes;
}

#endif // Q_OS_WIN

} // namespace

SecureBlob::SecureBlob(const QString &path) : _path(path) {
}

SecureBlob::~SecureBlob() {
	base::bytes::wipe(_key);
}

QString SecureBlob::keyringId() const {
	return QFileInfo(_path).absoluteFilePath();
}

std::optional<QByteArray> SecureBlob::keyringKey(bool create) const {
	if (!_key.isEmpty()) {
		return _key;
	}
	const auto keyring = Keyring::Default();
	if (!keyring) {
		return std::nullopt;
	}
	auto stored = QByteArray();
	switch (keyring->lookup(keyringId(), stored)) {
	case Keyring::Lookup::Found: {
		auto key = QByteArray::fromHex(stored);
		base::bytes::wipe(stored);
		if (key.size() != Crypto::kKeySize) {
			return std::nullopt;
		}
		_key = key;
		return key;
	}
	case Keyring::Lookup::Missing: {
		if (!create) {
			return std::nullopt;
		}
		auto key = Crypto::RandomBytes(Crypto::kKeySize);
		auto hex = key.toHex();
		const auto saved = keyring->store(
			keyringId(),
			u"Eleza Desktop (%1)"_s.arg(QFileInfo(_path).fileName()),
			hex);
		base::bytes::wipe(hex);
		if (!saved || key.size() != Crypto::kKeySize) {
			return std::nullopt;
		}
		_key = key;
		return key;
	}
	case Keyring::Lookup::Unavailable:
		break;
	}
	return std::nullopt;
}

std::optional<QByteArray> SecureBlob::writeToKeyring(const QByteArray &bytes) const {
	auto key = keyringKey(true);
	if (!key) {
		return std::nullopt;
	}
	const auto magic = QByteArray(kKeyringMagic, kMagicSize);
	const auto nonce = Crypto::RandomBytes(kWrapNonceSize);
	auto sealed = Crypto::AesGcmEncrypt(*key, nonce, magic, bytes);
	base::bytes::wipe(*key);
	if (sealed.isEmpty() || nonce.size() != kWrapNonceSize) {
		return std::nullopt;
	}
	return magic + nonce + sealed;
}

bool SecureBlob::exists() const {
	return QFileInfo::exists(_path);
}

std::optional<QByteArray> SecureBlob::read() const {
	auto stored = base::platform::ReadFile(_path);
	if (!stored || stored->size() < kMagicSize) {
		return std::nullopt;
	}
#ifndef Q_OS_WIN
	if (stored->startsWith(QByteArray(kKeyringMagic, kMagicSize))) {
		auto key = keyringKey(false);
		if (!key) {
			// Not the path: it has the name of the user's home folder in it.
			qWarning("Storage: the key of a secret is not in the Secret Service (any more).");
			base::bytes::wipe(*stored);
			return std::nullopt;
		}
		auto result = Crypto::AesGcmDecrypt(
			*key,
			stored->mid(kMagicSize, kWrapNonceSize),
			QByteArray(kKeyringMagic, kMagicSize),
			stored->mid(kMagicSize + kWrapNonceSize));
		base::bytes::wipe(*key);
		base::bytes::wipe(*stored);
		return result;
	}
#endif // !Q_OS_WIN
	if (!stored->startsWith(QByteArray(kMagic, kMagicSize))) {
		return std::nullopt;
	}
	auto result = Unseal(stored->mid(kMagicSize));
	base::bytes::wipe(*stored);
#ifndef Q_OS_WIN
	if (result && Keyring::Default()) {
		// Written while there was no Secret Service: into it now.
		if (write(*result)) {
			qWarning("Storage: a secret was moved from a plain file into the Secret Service.");
		}
	}
#endif // !Q_OS_WIN
	return result;
}

bool SecureBlob::write(const QByteArray &bytes) const {
#ifndef Q_OS_WIN
	if (Keyring::Default()) {
		if (auto stored = writeToKeyring(bytes)) {
			base::platform::EnsureDirectory(QFileInfo(_path).absolutePath(), true);
			const auto result = base::platform::WriteFileAtomic(_path, *stored, true);
			base::bytes::wipe(*stored);
			return result;
		}
		qWarning("Storage: the Secret Service did not take a key; this secret is kept in an owner-only file.");
	}
#endif // !Q_OS_WIN
	auto sealed = Seal(bytes);
	if (!sealed) {
		return false;
	}
	base::platform::EnsureDirectory(QFileInfo(_path).absolutePath(), true);
	auto stored = QByteArray(kMagic, kMagicSize) + *sealed;
	const auto result = base::platform::WriteFileAtomic(_path, stored, true);
	base::bytes::wipe(stored);
	base::bytes::wipe(*sealed);
	return result;
}

void SecureBlob::remove() const {
	// Overwrite before unlinking: the blob is tiny and holds secrets.
	auto file = QFile(_path);
	if (file.open(QIODevice::ReadWrite)) {
		const auto size = file.size();
		file.seek(0);
		file.write(QByteArray(int(qMin<qint64>(size, 1 << 20)), char(0)));
		file.close();
	}
	QFile::remove(_path);
#ifndef Q_OS_WIN
	base::bytes::wipe(_key);
	_key = QByteArray();
	if (const auto keyring = Keyring::Default()) {
		keyring->remove(keyringId());
	}
#endif // !Q_OS_WIN
}

} // namespace Storage
