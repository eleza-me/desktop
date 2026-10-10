// The device identity key. The backend (pkg/auth/deviceproof.go) accepts
// "ed25519" (raw 32-byte public key, 64-byte signature) and "p256"; the
// desktop client uses Ed25519: deterministic signatures, no ASN.1, native in
// OpenSSL 3. A proof is a signature over
//
//     "eleza/device-proof/v1" 0x00 <purpose> 0x00 <nonce>
//
// with purpose "refresh" (token refresh) or "ws_resume" (realtime challenge).
#pragma once

#include "base/basic_types.h"

namespace Crypto {

class DeviceKey {
public:
	DeviceKey() = default;

	// A fresh random key.
	[[nodiscard]] static DeviceKey Generate();
	// Restores a key from its stored 32-byte private part; invalid() on bad input.
	[[nodiscard]] static DeviceKey FromPrivate(const QByteArray &privateKey);

	[[nodiscard]] bool valid() const {
		return !_public.isEmpty();
	}
	// "ed25519": the `device_key.type` sent on sign-in.
	[[nodiscard]] static QString Type();
	// Raw private key for Storage::SecureBlob.
	[[nodiscard]] QByteArray privateKey() const {
		return _private;
	}
	[[nodiscard]] QByteArray publicKey() const {
		return _public;
	}
	// Standard base64 of the raw public key: the `device_key.key` sent on sign-in.
	[[nodiscard]] QString publicKeyBase64() const;

	// Signature over ProofMessage(purpose, nonce); empty when invalid().
	[[nodiscard]] QByteArray sign(const QString &purpose, const QByteArray &nonce) const;

	// The exact byte string a device signs.
	[[nodiscard]] static QByteArray ProofMessage(
		const QString &purpose,
		const QByteArray &nonce);
	// What the server does with a proof (used by tests and the interop peer check).
	[[nodiscard]] static bool Verify(
		const QByteArray &publicKey,
		const QString &purpose,
		const QByteArray &nonce,
		const QByteArray &signature);

private:
	QByteArray _private;
	QByteArray _public;

};

} // namespace Crypto
