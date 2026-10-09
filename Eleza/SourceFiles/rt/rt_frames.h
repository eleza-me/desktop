// Realtime v2 framing. An Envelope travels as one or more records; each
// record is one Noise transport message (at most 65535 bytes). Plaintext:
//
//   [flags:1][chunk_len:2][total:3][chunk...][zero padding]
//
// flags bit0 = last chunk; chunk_len = envelope bytes in this record; total =
// the envelope's full length; the plaintext is padded to a multiple of 64 so
// record sizes do not leak message lengths. Both peers rekey their
// CipherState after every 2^16 records per direction.
#pragma once

#include "rt/rt_noise.h"

namespace Rt::Frames {

inline constexpr auto kHeaderSize = 6;
inline constexpr auto kPadding = 64;
inline constexpr auto kRecordMax = Noise::kMaxMessage - Noise::kTagSize;
inline constexpr auto kChunkMax = kRecordMax - kHeaderSize - kPadding;
inline constexpr auto kMaxFrame = 256 << 10;
inline constexpr auto kRekeyEvery = quint32(1) << 16;

// Encrypts one envelope into its records; nullopt when it exceeds kMaxFrame
// or the cipher failed.
[[nodiscard]] std::optional<QVector<QByteArray>> EncodeRecords(
	Noise::CipherState &cipher,
	const QByteArray &envelope);

// Decrypts records and reassembles envelopes.
class RecordDecoder {
public:
	enum class Status {
		Failed,   // malformed record or failed authentication: drop the connection
		Partial,  // consumed, the envelope continues in the next record
		Complete, // `envelope` holds a whole envelope
	};
	struct Result {
		Status status = Status::Failed;
		QByteArray envelope;
	};

	explicit RecordDecoder(Noise::CipherState *cipher = nullptr) : _cipher(cipher) {
	}
	void setCipher(Noise::CipherState *cipher) {
		_cipher = cipher;
	}

	[[nodiscard]] Result record(const QByteArray &ciphertext);

private:
	Noise::CipherState *_cipher = nullptr;
	QByteArray _buffer;
	int _want = 0;
	bool _collecting = false;

};

} // namespace Rt::Frames
