#include "crypto/crypto_device_key.h"

#include "base/bytes.h"
#include "crypto/crypto_primitives.h"

namespace Crypto {

DeviceKey DeviceKey::Generate() {
	return FromPrivate(Ed25519GeneratePrivate());
}

DeviceKey DeviceKey::FromPrivate(const QByteArray &privateKey) {
	auto result = DeviceKey();
	const auto publicKey = Ed25519PublicFromPrivate(privateKey);
	if (!publicKey.isEmpty()) {
		result._private = privateKey;
		result._public = publicKey;
	}
	return result;
}

QString DeviceKey::Type() {
	return QStringLiteral("ed25519");
}

QString DeviceKey::publicKeyBase64() const {
	return base::bytes::toBase64(_public);
}

QByteArray DeviceKey::sign(const QString &purpose, const QByteArray &nonce) const {
	return valid()
		? Ed25519Sign(_private, ProofMessage(purpose, nonce))
		: QByteArray();
}

QByteArray DeviceKey::ProofMessage(const QString &purpose, const QByteArray &nonce) {
	auto result = QByteArray("eleza/device-proof/v1");
	result.append('\0');
	result.append(purpose.toUtf8());
	result.append('\0');
	result.append(nonce);
	return result;
}

bool DeviceKey::Verify(
		const QByteArray &publicKey,
		const QString &purpose,
		const QByteArray &nonce,
		const QByteArray &signature) {
	return Ed25519Verify(publicKey, ProofMessage(purpose, nonce), signature);
}

} // namespace Crypto
