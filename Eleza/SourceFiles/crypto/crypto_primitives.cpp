#include "crypto/crypto_primitives.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace Crypto {
namespace {

struct PkeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
using PkeyPointer = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct PkeyContextDeleter {
	void operator()(EVP_PKEY_CTX *context) const {
		EVP_PKEY_CTX_free(context);
	}
};
using PkeyContextPointer = std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter>;

struct CipherContextDeleter {
	void operator()(EVP_CIPHER_CTX *context) const {
		EVP_CIPHER_CTX_free(context);
	}
};
using CipherContextPointer = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;

struct DigestContextDeleter {
	void operator()(EVP_MD_CTX *context) const {
		EVP_MD_CTX_free(context);
	}
};
using DigestContextPointer = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

[[nodiscard]] const uchar *Bytes(const QByteArray &data) {
	return reinterpret_cast<const uchar*>(data.constData());
}

[[nodiscard]] uchar *Bytes(QByteArray &data) {
	return reinterpret_cast<uchar*>(data.data());
}

[[nodiscard]] QByteArray RawPublic(EVP_PKEY *key) {
	auto size = size_t(kKeySize);
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	if (EVP_PKEY_get_raw_public_key(key, Bytes(result), &size) != 1
		|| size != size_t(kKeySize)) {
		return QByteArray();
	}
	return result;
}

[[nodiscard]] QByteArray PublicFromPrivate(int type, const QByteArray &privateKey) {
	if (privateKey.size() != kKeySize) {
		return QByteArray();
	}
	const auto key = PkeyPointer(EVP_PKEY_new_raw_private_key(
		type,
		nullptr,
		Bytes(privateKey),
		size_t(privateKey.size())));
	return key ? RawPublic(key.get()) : QByteArray();
}

} // namespace

QByteArray RandomBytes(int size) {
	auto result = QByteArray(size, Qt::Uninitialized);
	if (size > 0 && RAND_bytes(Bytes(result), size) != 1) {
		qFatal("Crypto: the system random generator failed.");
	}
	return result;
}

QByteArray Sha256(const QByteArray &data) {
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	auto size = 0U;
	if (EVP_Digest(
			data.constData(),
			size_t(data.size()),
			Bytes(result),
			&size,
			EVP_sha256(),
			nullptr) != 1 || size != uint(kKeySize)) {
		return QByteArray();
	}
	return result;
}

QByteArray HmacSha256(const QByteArray &key, const QByteArray &data) {
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	auto size = 0U;
	// HMAC() refuses a null key pointer even for an empty key.
	static const char kEmpty[1] = { 0 };
	const auto got = HMAC(
		EVP_sha256(),
		key.isEmpty() ? kEmpty : key.constData(),
		int(key.size()),
		Bytes(data),
		size_t(data.size()),
		Bytes(result),
		&size);
	if (!got || size != uint(kKeySize)) {
		return QByteArray();
	}
	return result;
}

Sha256Hasher::Sha256Hasher() : _context(EVP_MD_CTX_new()) {
	EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(_context), EVP_sha256(), nullptr);
}

Sha256Hasher::~Sha256Hasher() {
	EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(_context));
}

void Sha256Hasher::feed(const QByteArray &data) {
	feed(data.constData(), data.size());
}

void Sha256Hasher::feed(const char *data, qsizetype size) {
	if (size > 0) {
		EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(_context), data, size_t(size));
	}
}

QByteArray Sha256Hasher::result() {
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	auto size = 0U;
	if (EVP_DigestFinal_ex(
			static_cast<EVP_MD_CTX*>(_context),
			Bytes(result),
			&size) != 1 || size != uint(kKeySize)) {
		return QByteArray();
	}
	return result;
}

HkdfOutput NoiseHkdf(
		const QByteArray &chainingKey,
		const QByteArray &inputKeyMaterial,
		int outputs) {
	auto result = HkdfOutput();
	auto tempKey = HmacSha256(chainingKey, inputKeyMaterial);
	result.first = HmacSha256(tempKey, QByteArray(1, char(1)));
	result.second = HmacSha256(tempKey, result.first + QByteArray(1, char(2)));
	if (outputs >= 3) {
		result.third = HmacSha256(tempKey, result.second + QByteArray(1, char(3)));
	}
	OPENSSL_cleanse(tempKey.data(), size_t(tempKey.size()));
	return result;
}

QByteArray X25519GeneratePrivate() {
	return RandomBytes(kKeySize);
}

QByteArray X25519PublicFromPrivate(const QByteArray &privateKey) {
	return PublicFromPrivate(EVP_PKEY_X25519, privateKey);
}

QByteArray X25519Shared(const QByteArray &privateKey, const QByteArray &peerPublicKey) {
	if (privateKey.size() != kKeySize || peerPublicKey.size() != kKeySize) {
		return QByteArray();
	}
	const auto mine = PkeyPointer(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_X25519,
		nullptr,
		Bytes(privateKey),
		size_t(kKeySize)));
	const auto peer = PkeyPointer(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_X25519,
		nullptr,
		Bytes(peerPublicKey),
		size_t(kKeySize)));
	if (!mine || !peer) {
		return QByteArray();
	}
	const auto context = PkeyContextPointer(EVP_PKEY_CTX_new(mine.get(), nullptr));
	if (!context
		|| EVP_PKEY_derive_init(context.get()) != 1
		|| EVP_PKEY_derive_set_peer(context.get(), peer.get()) != 1) {
		return QByteArray();
	}
	auto size = size_t(kKeySize);
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	if (EVP_PKEY_derive(context.get(), Bytes(result), &size) != 1
		|| size != size_t(kKeySize)) {
		return QByteArray();
	}
	return result;
}

QByteArray ChaChaPolyEncrypt(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &associatedData,
		const QByteArray &plaintext) {
	if (key.size() != kKeySize || nonce.size() != kNonceSize) {
		return QByteArray();
	}
	const auto context = CipherContextPointer(EVP_CIPHER_CTX_new());
	if (!context
		|| EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_EncryptInit_ex(context.get(), nullptr, nullptr, Bytes(key), Bytes(nonce)) != 1) {
		return QByteArray();
	}
	auto written = 0;
	if (!associatedData.isEmpty()
		&& EVP_EncryptUpdate(
			context.get(),
			nullptr,
			&written,
			Bytes(associatedData),
			int(associatedData.size())) != 1) {
		return QByteArray();
	}
	auto result = QByteArray(plaintext.size() + kTagSize, Qt::Uninitialized);
	auto total = 0;
	if (!plaintext.isEmpty()) {
		if (EVP_EncryptUpdate(
				context.get(),
				Bytes(result),
				&written,
				Bytes(plaintext),
				int(plaintext.size())) != 1) {
			return QByteArray();
		}
		total += written;
	}
	if (EVP_EncryptFinal_ex(context.get(), Bytes(result) + total, &written) != 1) {
		return QByteArray();
	}
	total += written;
	if (total != plaintext.size()
		|| EVP_CIPHER_CTX_ctrl(
			context.get(),
			EVP_CTRL_AEAD_GET_TAG,
			kTagSize,
			Bytes(result) + total) != 1) {
		return QByteArray();
	}
	return result;
}

std::optional<QByteArray> ChaChaPolyDecrypt(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &associatedData,
		const QByteArray &ciphertext) {
	if (key.size() != kKeySize
		|| nonce.size() != kNonceSize
		|| ciphertext.size() < kTagSize) {
		return std::nullopt;
	}
	const auto size = int(ciphertext.size()) - kTagSize;
	const auto context = CipherContextPointer(EVP_CIPHER_CTX_new());
	if (!context
		|| EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_DecryptInit_ex(context.get(), nullptr, nullptr, Bytes(key), Bytes(nonce)) != 1) {
		return std::nullopt;
	}
	// The tag is copied because the control call takes a mutable pointer.
	auto tag = ciphertext.mid(size, kTagSize);
	if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, kTagSize, tag.data()) != 1) {
		return std::nullopt;
	}
	auto written = 0;
	if (!associatedData.isEmpty()
		&& EVP_DecryptUpdate(
			context.get(),
			nullptr,
			&written,
			Bytes(associatedData),
			int(associatedData.size())) != 1) {
		return std::nullopt;
	}
	auto result = QByteArray(size, Qt::Uninitialized);
	auto total = 0;
	if (size > 0) {
		if (EVP_DecryptUpdate(
				context.get(),
				Bytes(result),
				&written,
				Bytes(ciphertext),
				size) != 1) {
			return std::nullopt;
		}
		total += written;
	}
	if (EVP_DecryptFinal_ex(context.get(), Bytes(result) + total, &written) != 1) {
		return std::nullopt;
	}
	if (result.isNull()) {
		result = QByteArray("", 0);
	}
	return result;
}

QByteArray Ed25519GeneratePrivate() {
	return RandomBytes(kKeySize);
}

QByteArray Ed25519PublicFromPrivate(const QByteArray &privateKey) {
	return PublicFromPrivate(EVP_PKEY_ED25519, privateKey);
}

QByteArray Ed25519Sign(const QByteArray &privateKey, const QByteArray &message) {
	if (privateKey.size() != kKeySize) {
		return QByteArray();
	}
	const auto key = PkeyPointer(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_ED25519,
		nullptr,
		Bytes(privateKey),
		size_t(kKeySize)));
	const auto context = DigestContextPointer(EVP_MD_CTX_new());
	if (!key
		|| !context
		|| EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
		return QByteArray();
	}
	auto size = size_t(64);
	auto result = QByteArray(64, Qt::Uninitialized);
	if (EVP_DigestSign(
			context.get(),
			Bytes(result),
			&size,
			Bytes(message),
			size_t(message.size())) != 1 || size != 64) {
		return QByteArray();
	}
	return result;
}

bool Ed25519Verify(
		const QByteArray &publicKey,
		const QByteArray &message,
		const QByteArray &signature) {
	if (publicKey.size() != kKeySize || signature.size() != 64) {
		return false;
	}
	const auto key = PkeyPointer(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		Bytes(publicKey),
		size_t(kKeySize)));
	const auto context = DigestContextPointer(EVP_MD_CTX_new());
	if (!key
		|| !context
		|| EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
		return false;
	}
	return EVP_DigestVerify(
		context.get(),
		Bytes(signature),
		size_t(signature.size()),
		Bytes(message),
		size_t(message.size())) == 1;
}

} // namespace Crypto
