// Noise_NK_25519_ChaChaPoly_SHA256 (Noise Protocol Framework rev. 34).
//
// The client is the initiator: it knows the gateway's static X25519 key in
// advance (published at GET /v2/keys) and stays anonymous at this layer; the
// access token inside the first payload authenticates the user.
//
//   -> e, es   [payload]      message 1: client -> server
//   <- e, ee   [payload]      message 2: server -> client
//   then two transport CipherStates (send, recv), rekeyed by the framing layer.
//
// The Responder exists for the unit tests (the backend's test vectors).
#pragma once

#include "base/basic_types.h"

namespace Rt::Noise {

inline constexpr auto kKeySize = 32;
inline constexpr auto kTagSize = 16;
inline constexpr auto kMaxMessage = 65535;

[[nodiscard]] QByteArray ProtocolName();

// One direction of an established session (spec section 5.1).
class CipherState {
public:
	CipherState() = default;
	CipherState(const CipherState &other) = default;
	CipherState &operator=(const CipherState &other);
	// The key is wiped, not just released.
	~CipherState();

	void initializeKey(const QByteArray &key);
	[[nodiscard]] bool hasKey() const {
		return _hasKey;
	}

	// nullopt: nonce exhausted or (decrypt) authentication failed. Without a
	// key both return their input unchanged, as the spec defines.
	[[nodiscard]] std::optional<QByteArray> encrypt(
		const QByteArray &associatedData,
		const QByteArray &plaintext);
	[[nodiscard]] std::optional<QByteArray> decrypt(
		const QByteArray &associatedData,
		const QByteArray &ciphertext);

	// k = ENCRYPT(k, 2^64-1, "", zeros[32])[:32] (spec section 4.2); resets count().
	void rekey();

	// Messages since the last rekey (the framing layer's rekey policy).
	[[nodiscard]] quint32 count() const {
		return _count;
	}

private:
	QByteArray _key;
	quint64 _nonce = 0;
	quint32 _count = 0;
	bool _hasKey = false;

};

struct Session {
	CipherState send; // encrypts our records
	CipherState recv; // decrypts the peer's records
	QByteArray handshakeHash;
};

class SymmetricState {
public:
	SymmetricState();
	SymmetricState(const SymmetricState &other) = default;
	SymmetricState &operator=(const SymmetricState &other) = default;
	~SymmetricState();

	void mixKey(const QByteArray &inputKeyMaterial);
	void mixHash(const QByteArray &data);
	[[nodiscard]] std::optional<QByteArray> encryptAndHash(const QByteArray &plaintext);
	[[nodiscard]] std::optional<QByteArray> decryptAndHash(const QByteArray &ciphertext);
	// First: initiator -> responder, second: responder -> initiator.
	[[nodiscard]] std::pair<CipherState, CipherState> split() const;
	[[nodiscard]] QByteArray hash() const {
		return _hash;
	}

private:
	CipherState _cipher;
	QByteArray _chainingKey;
	QByteArray _hash;

};

// The client side of an NK handshake.
class Initiator {
public:
	// remoteStatic: the server's static public key (32 bytes).
	// prologue: context both sides must agree on ("eleza-rt-v2/<key id>").
	// ephemeral: a fixed ephemeral private key (tests only).
	Initiator(
		const QByteArray &remoteStatic,
		const QByteArray &prologue,
		const QByteArray &ephemeral = QByteArray());
	~Initiator();

	[[nodiscard]] bool valid() const {
		return _valid;
	}

	// "-> e, es" with payload; nullopt on misuse.
	[[nodiscard]] std::optional<QByteArray> writeMessage1(const QByteArray &payload);
	// "<- e, ee": returns the payload and establishes session().
	[[nodiscard]] std::optional<QByteArray> readMessage2(const QByteArray &message);

	[[nodiscard]] bool established() const {
		return _step == 2;
	}
	[[nodiscard]] Session &session() {
		return _session;
	}

private:
	SymmetricState _symmetric;
	QByteArray _remoteStatic;
	QByteArray _ephemeralPrivate;
	Session _session;
	int _step = 0;
	bool _valid = false;

};

// The server side (tests): mirrors the backend's pkg/noise responder.
class Responder {
public:
	Responder(
		const QByteArray &staticPrivate,
		const QByteArray &prologue,
		const QByteArray &ephemeral = QByteArray());

	[[nodiscard]] std::optional<QByteArray> readMessage1(const QByteArray &message);
	[[nodiscard]] std::optional<QByteArray> writeMessage2(const QByteArray &payload);
	[[nodiscard]] Session &session() {
		return _session;
	}

private:
	SymmetricState _symmetric;
	QByteArray _staticPrivate;
	QByteArray _ephemeralPrivate;
	QByteArray _remoteEphemeral;
	Session _session;

};

} // namespace Rt::Noise
