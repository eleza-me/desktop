// Thin wrappers over OpenSSL 3 EVP: exactly the primitives the Noise NK
// handshake, uploads and the device proof need. Every function reports
// failure through its return value (no exceptions).
#pragma once

#include "base/basic_types.h"

namespace Crypto {

inline constexpr auto kKeySize = 32;       // X25519 / ChaCha20 key, SHA-256 output
inline constexpr auto kTagSize = 16;       // Poly1305 tag
inline constexpr auto kNonceSize = 12;     // RFC 8439 nonce

// Cryptographically secure random bytes (RAND_bytes).
[[nodiscard]] QByteArray RandomBytes(int size);

[[nodiscard]] QByteArray Sha256(const QByteArray &data);
[[nodiscard]] QByteArray HmacSha256(const QByteArray &key, const QByteArray &data);

// Incremental SHA-256 (file hashing off the main thread).
class Sha256Hasher {
public:
	Sha256Hasher();
	~Sha256Hasher();
	Sha256Hasher(const Sha256Hasher &) = delete;
	Sha256Hasher &operator=(const Sha256Hasher &) = delete;

	void feed(const QByteArray &data);
	void feed(const char *data, qsizetype size);
	[[nodiscard]] QByteArray result();

private:
	void *_context = nullptr;

};

// Noise HKDF (rev. 34 section 4.3): two or three 32-byte outputs from a
// chaining key and input key material, HMAC-SHA256 based.
struct HkdfOutput {
	QByteArray first;
	QByteArray second;
	QByteArray third; // empty when two outputs were requested
};
[[nodiscard]] HkdfOutput NoiseHkdf(
	const QByteArray &chainingKey,
	const QByteArray &inputKeyMaterial,
	int outputs);

// X25519 (RFC 7748). Empty results on malformed input.
[[nodiscard]] QByteArray X25519GeneratePrivate();
[[nodiscard]] QByteArray X25519PublicFromPrivate(const QByteArray &privateKey);
[[nodiscard]] QByteArray X25519Shared(
	const QByteArray &privateKey,
	const QByteArray &peerPublicKey);

// ChaCha20-Poly1305 AEAD (RFC 8439). Encrypt returns ciphertext || tag;
// Decrypt returns nullopt when authentication fails.
[[nodiscard]] QByteArray ChaChaPolyEncrypt(
	const QByteArray &key,
	const QByteArray &nonce,
	const QByteArray &associatedData,
	const QByteArray &plaintext);
[[nodiscard]] std::optional<QByteArray> ChaChaPolyDecrypt(
	const QByteArray &key,
	const QByteArray &nonce,
	const QByteArray &associatedData,
	const QByteArray &ciphertext);

// Ed25519 (RFC 8032), raw 32-byte keys and 64-byte signatures.
[[nodiscard]] QByteArray Ed25519GeneratePrivate();
[[nodiscard]] QByteArray Ed25519PublicFromPrivate(const QByteArray &privateKey);
[[nodiscard]] QByteArray Ed25519Sign(
	const QByteArray &privateKey,
	const QByteArray &message);
[[nodiscard]] bool Ed25519Verify(
	const QByteArray &publicKey,
	const QByteArray &message,
	const QByteArray &signature);

} // namespace Crypto
