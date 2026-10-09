#include "rt/rt_frames.h"

#include <algorithm>

namespace Rt::Frames {

std::optional<QVector<QByteArray>> EncodeRecords(
		Noise::CipherState &cipher,
		const QByteArray &envelope) {
	if (envelope.size() > kMaxFrame) {
		return std::nullopt;
	}
	auto result = QVector<QByteArray>();
	const auto total = int(envelope.size());
	auto offset = 0;
	auto first = true;
	while (first || offset < total) {
		first = false;
		const auto size = std::min(total - offset, int(kChunkMax));
		const auto last = (offset + size >= total);
		auto length = kHeaderSize + size;
		if (const auto rest = length % kPadding) {
			length += kPadding - rest;
		}
		auto plaintext = QByteArray(length, char(0));
		plaintext[0] = char(last ? 1 : 0);
		plaintext[1] = char((size >> 8) & 0xFF);
		plaintext[2] = char(size & 0xFF);
		plaintext[3] = char((total >> 16) & 0xFF);
		plaintext[4] = char((total >> 8) & 0xFF);
		plaintext[5] = char(total & 0xFF);
		if (size > 0) {
			std::copy_n(
				envelope.constData() + offset,
				size,
				plaintext.data() + kHeaderSize);
		}
		auto record = cipher.encrypt(QByteArray(), plaintext);
		if (!record) {
			return std::nullopt;
		}
		result.push_back(std::move(*record));
		if (cipher.count() >= kRekeyEvery) {
			cipher.rekey();
		}
		offset += size;
	}
	return result;
}

RecordDecoder::Result RecordDecoder::record(const QByteArray &ciphertext) {
	const auto failed = [&] {
		_buffer.clear();
		_collecting = false;
		_want = 0;
		return Result();
	};
	if (!_cipher) {
		return failed();
	}
	const auto decrypted = _cipher->decrypt(QByteArray(), ciphertext);
	if (!decrypted) {
		return failed();
	}
	if (_cipher->count() >= kRekeyEvery) {
		_cipher->rekey();
	}
	const auto &plaintext = *decrypted;
	if (plaintext.size() < kHeaderSize) {
		return failed();
	}
	const auto byte = [&](int index) {
		return int(uchar(plaintext[index]));
	};
	const auto flags = byte(0);
	const auto size = (byte(1) << 8) | byte(2);
	const auto total = (byte(3) << 16) | (byte(4) << 8) | byte(5);
	if (total > kMaxFrame || size > plaintext.size() - kHeaderSize) {
		return failed();
	}
	if (!_collecting) {
		_collecting = true;
		_want = total;
		_buffer.clear();
		_buffer.reserve(total);
	} else if (total != _want) {
		return failed();
	}
	if (_buffer.size() + size > _want) {
		return failed();
	}
	_buffer.append(plaintext.constData() + kHeaderSize, size);
	if (!(flags & 1)) {
		return { Status::Partial, QByteArray() };
	} else if (_buffer.size() != _want) {
		return failed();
	}
	auto result = Result{ Status::Complete, std::move(_buffer) };
	if (result.envelope.isNull()) {
		result.envelope = QByteArray("", 0);
	}
	_buffer = QByteArray();
	_collecting = false;
	_want = 0;
	return result;
}

} // namespace Rt::Frames
