#include "rt/rt_noise.h"

#include "base/bytes.h"
#include "crypto/crypto_primitives.h"

#include <limits>

namespace Rt::Noise {
namespace {

constexpr auto kMaxNonce = std::numeric_limits<quint64>::max();

// 12-byte nonce: 4 zero bytes, then the counter little-endian (spec section 12.3).
[[nodiscard]] QByteArray NonceBytes(quint64 value) {
	auto result = QByteArray(Crypto::kNonceSize, char(0));
	for (auto i = 4; i != 12; ++i) {
		result[i] = char(value & 0xFFU);
		value >>= 8;
	}
	return result;
}

} // namespace

QByteArray ProtocolName() {
	return QByteArrayLiteral("Noise_NK_25519_ChaChaPoly_SHA256");
}

CipherState::CipherState(const CipherState &other)
: _key(base::bytes::copy(other._key))
, _nonce(other._nonce)
, _count(other._count)
, _hasKey(other._hasKey) {
}

CipherState &CipherState::operator=(const CipherState &other) {
	if (this != &other) {
		base::bytes::wipe(_key);
		_key = base::bytes::copy(other._key);
		_nonce = other._nonce;
		_count = other._count;
		_hasKey = other._hasKey;
	}
	return *this;
}

CipherState::~CipherState() {
	base::bytes::wipe(_key);
}

void CipherState::initializeKey(const QByteArray &key) {
	base::bytes::wipe(_key);
	_key = base::bytes::copy(key);
	_nonce = 0;
	_count = 0;
	_hasKey = true;
}

std::optional<QByteArray> CipherState::encrypt(
		const QByteArray &associatedData,
		const QByteArray &plaintext) {
	if (!_hasKey) {
		return plaintext;
	} else if (_nonce == kMaxNonce) {
		return std::nullopt;
	}
	auto result = Crypto::ChaChaPolyEncrypt(
		_key,
		NonceBytes(_nonce),
		associatedData,
		plaintext);
	if (result.isEmpty()) {
		return std::nullopt;
	}
	++_nonce;
	++_count;
	return result;
}

std::optional<QByteArray> CipherState::decrypt(
		const QByteArray &associatedData,
		const QByteArray &ciphertext) {
	if (!_hasKey) {
		return ciphertext;
	} else if (_nonce == kMaxNonce) {
		return std::nullopt;
	}
	auto result = Crypto::ChaChaPolyDecrypt(
		_key,
		NonceBytes(_nonce),
		associatedData,
		ciphertext);
	if (!result) {
		return std::nullopt;
	}
	++_nonce;
	++_count;
	return result;
}

void CipherState::rekey() {
	auto output = Crypto::ChaChaPolyEncrypt(
		_key,
		NonceBytes(kMaxNonce),
		QByteArray(),
		QByteArray(kKeySize, char(0)));
	base::bytes::wipe(_key);
	// A failed primitive leaves no key: every later record fails closed.
	_key = output.left(kKeySize);
	base::bytes::wipe(output);
	_count = 0;
}

SymmetricState::SymmetricState() {
	const auto name = ProtocolName();
	if (name.size() <= 32) {
		_hash = name + QByteArray(32 - name.size(), char(0));
	} else {
		_hash = Crypto::Sha256(name);
	}
	_chainingKey = _hash;
}

SymmetricState::SymmetricState(const SymmetricState &other)
: _cipher(other._cipher)
, _chainingKey(base::bytes::copy(other._chainingKey))
, _hash(other._hash) {
}

SymmetricState &SymmetricState::operator=(const SymmetricState &other) {
	if (this != &other) {
		_cipher = other._cipher;
		base::bytes::wipe(_chainingKey);
		_chainingKey = base::bytes::copy(other._chainingKey);
		_hash = other._hash;
	}
	return *this;
}

SymmetricState::~SymmetricState() {
	base::bytes::wipe(_chainingKey);
}

void SymmetricState::mixKey(const QByteArray &inputKeyMaterial) {
	auto output = Crypto::NoiseHkdf(_chainingKey, inputKeyMaterial, 2);
	base::bytes::wipe(_chainingKey);
	_chainingKey = base::bytes::copy(output.first);
	_cipher.initializeKey(output.second);
	base::bytes::wipe(output.first);
	base::bytes::wipe(output.second);
}

void SymmetricState::mixHash(const QByteArray &data) {
	_hash = Crypto::Sha256(_hash + data);
}

std::optional<QByteArray> SymmetricState::encryptAndHash(const QByteArray &plaintext) {
	auto result = _cipher.encrypt(_hash, plaintext);
	if (result) {
		mixHash(*result);
	}
	return result;
}

std::optional<QByteArray> SymmetricState::decryptAndHash(const QByteArray &ciphertext) {
	auto result = _cipher.decrypt(_hash, ciphertext);
	if (result) {
		mixHash(ciphertext);
	}
	return result;
}

std::pair<CipherState, CipherState> SymmetricState::split() const {
	auto output = Crypto::NoiseHkdf(_chainingKey, QByteArray(), 2);
	auto result = std::pair<CipherState, CipherState>();
	result.first.initializeKey(output.first);
	result.second.initializeKey(output.second);
	base::bytes::wipe(output.first);
	base::bytes::wipe(output.second);
	return result;
}

Initiator::Initiator(
	const QByteArray &remoteStatic,
	const QByteArray &prologue,
	const QByteArray &ephemeral)
: _remoteStatic(remoteStatic)
, _ephemeralPrivate(base::bytes::copy(ephemeral))
, _valid(remoteStatic.size() == kKeySize
	&& (ephemeral.isEmpty() || ephemeral.size() == kKeySize)) {
	_symmetric.mixHash(prologue);
	_symmetric.mixHash(_remoteStatic); // NK pre-message: <- s
}

Initiator::~Initiator() {
	base::bytes::wipe(_ephemeralPrivate);
}

std::optional<QByteArray> Initiator::writeMessage1(const QByteArray &payload) {
	if (!_valid || _step != 0) {
		return std::nullopt;
	}
	if (_ephemeralPrivate.isEmpty()) {
		_ephemeralPrivate = Crypto::X25519GeneratePrivate();
	}
	const auto ephemeralPublic = Crypto::X25519PublicFromPrivate(_ephemeralPrivate);
	auto shared = Crypto::X25519Shared(_ephemeralPrivate, _remoteStatic);
	if (ephemeralPublic.isEmpty() || shared.isEmpty()) {
		return std::nullopt;
	}
	_symmetric.mixHash(ephemeralPublic);
	_symmetric.mixKey(shared);
	base::bytes::wipe(shared);
	const auto ciphertext = _symmetric.encryptAndHash(payload);
	if (!ciphertext) {
		return std::nullopt;
	}
	_step = 1;
	return ephemeralPublic + *ciphertext;
}

std::optional<QByteArray> Initiator::readMessage2(const QByteArray &message) {
	if (_step != 1 || message.size() < kKeySize + kTagSize) {
		return std::nullopt;
	}
	const auto remoteEphemeral = message.left(kKeySize);
	auto shared = Crypto::X25519Shared(_ephemeralPrivate, remoteEphemeral);
	if (shared.isEmpty()) {
		return std::nullopt;
	}
	_symmetric.mixHash(remoteEphemeral);
	_symmetric.mixKey(shared);
	base::bytes::wipe(shared);
	auto payload = _symmetric.decryptAndHash(message.mid(kKeySize));
	if (!payload) {
		// The transcript is spent: no second message is tried against it.
		_step = -1;
		base::bytes::wipe(_ephemeralPrivate);
		return std::nullopt;
	}
	auto [send, recv] = _symmetric.split();
	_session.send = send;
	_session.recv = recv;
	_session.handshakeHash = _symmetric.hash();
	_step = 2;
	base::bytes::wipe(_ephemeralPrivate);
	return payload;
}

Responder::Responder(
	const QByteArray &staticPrivate,
	const QByteArray &prologue,
	const QByteArray &ephemeral)
: _staticPrivate(base::bytes::copy(staticPrivate))
, _ephemeralPrivate(base::bytes::copy(ephemeral)) {
	_symmetric.mixHash(prologue);
	_symmetric.mixHash(Crypto::X25519PublicFromPrivate(_staticPrivate));
}

std::optional<QByteArray> Responder::readMessage1(const QByteArray &message) {
	if (message.size() < kKeySize + kTagSize) {
		return std::nullopt;
	}
	_remoteEphemeral = message.left(kKeySize);
	const auto shared = Crypto::X25519Shared(_staticPrivate, _remoteEphemeral);
	if (shared.isEmpty()) {
		return std::nullopt;
	}
	_symmetric.mixHash(_remoteEphemeral);
	_symmetric.mixKey(shared);
	return _symmetric.decryptAndHash(message.mid(kKeySize));
}

std::optional<QByteArray> Responder::writeMessage2(const QByteArray &payload) {
	if (_remoteEphemeral.isEmpty()) {
		return std::nullopt;
	}
	if (_ephemeralPrivate.isEmpty()) {
		_ephemeralPrivate = Crypto::X25519GeneratePrivate();
	}
	const auto ephemeralPublic = Crypto::X25519PublicFromPrivate(_ephemeralPrivate);
	const auto shared = Crypto::X25519Shared(_ephemeralPrivate, _remoteEphemeral);
	if (ephemeralPublic.isEmpty() || shared.isEmpty()) {
		return std::nullopt;
	}
	_symmetric.mixHash(ephemeralPublic);
	_symmetric.mixKey(shared);
	const auto ciphertext = _symmetric.encryptAndHash(payload);
	if (!ciphertext) {
		return std::nullopt;
	}
	// The responder sends with the second state and receives with the first.
	auto [first, second] = _symmetric.split();
	_session.send = second;
	_session.recv = first;
	_session.handshakeHash = _symmetric.hash();
	return ephemeralPublic + *ciphertext;
}

} // namespace Rt::Noise
