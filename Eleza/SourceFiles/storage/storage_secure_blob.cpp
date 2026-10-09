#include "storage/storage_secure_blob.h"

#include "base/bytes.h"
#include "base/platform_file.h"

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
constexpr char kMagic[] = "ELZB1";
constexpr auto kMagicSize = int(sizeof(kMagic) - 1);

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

bool SecureBlob::exists() const {
	return QFileInfo::exists(_path);
}

std::optional<QByteArray> SecureBlob::read() const {
	auto stored = base::platform::ReadFile(_path);
	if (!stored
		|| stored->size() < kMagicSize
		|| !stored->startsWith(QByteArray(kMagic, kMagicSize))) {
		return std::nullopt;
	}
	auto result = Unseal(stored->mid(kMagicSize));
	base::bytes::wipe(*stored);
	return result;
}

bool SecureBlob::write(const QByteArray &bytes) const {
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
}

} // namespace Storage
