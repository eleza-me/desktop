// The realtime framing and the handshake reader.
//
// The record reassembly is fuzzed in the clear: a CipherState without a key
// passes its input through (as Noise defines), so the bytes go straight to the
// header / chunk / total checks. The input is cut into records by two-byte
// lengths. Separately: message 2 of the handshake against a fixed initiator,
// and a keyed decoder that must refuse everything it did not encrypt.
#include "fuzz_driver.h"

#include "rt/rt_frames.h"
#include "rt/rt_noise.h"
#include "rt/rt_proto.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	auto input = Fuzz::Input(data, size);
	const auto mode = input.byte() % 4;
	if (mode == 0 || mode == 1) {
		auto clear = Rt::Noise::CipherState();
		auto decoder = Rt::Frames::RecordDecoder(&clear);
		auto total = qsizetype(0);
		while (!input.empty()) {
			const auto length = (size_t(input.byte()) << 8) | input.byte();
			const auto result = decoder.record(input.bytes(length));
			switch (result.status) {
			case Rt::Frames::RecordDecoder::Status::Failed:
				return 0;
			case Rt::Frames::RecordDecoder::Status::Partial:
				break;
			case Rt::Frames::RecordDecoder::Status::Complete:
				if (result.envelope.size() > Rt::Frames::kMaxFrame) {
					qFatal("fuzz_frames: an envelope above the frame limit");
				}
				total += result.envelope.size();
				(void)Rt::Proto::Decode(QLatin1StringView("Envelope"), result.envelope);
				break;
			}
		}
		(void)total;
	} else if (mode == 2) {
		const auto server = QByteArray(32, char(7));
		const auto serverPublic = QByteArray::fromHex(
			"6ac378aa14d587c46de508bd15d493c2124f6a6525e73a79481d39ba98ac650d");
		auto initiator = Rt::Noise::Initiator(
			serverPublic,
			"eleza-rt-v2/1"_ba,
			QByteArray(32, char(9)));
		(void)server;
		if (!initiator.writeMessage1("auth"_ba)) {
			return 0;
		}
		if (initiator.readMessage2(input.rest())) {
			qFatal("fuzz_frames: a forged message 2 was accepted");
		}
		if (initiator.established()) {
			qFatal("fuzz_frames: established without a valid message 2");
		}
	} else {
		auto send = Rt::Noise::CipherState();
		auto receive = Rt::Noise::CipherState();
		send.initializeKey(QByteArray(32, char(1)));
		receive.initializeKey(QByteArray(32, char(1)));
		auto decoder = Rt::Frames::RecordDecoder(&receive);
		// A genuine record, then the fuzzed one: the second must fail and the
		// decoder must stay failed-closed for that record only.
		const auto good = Rt::Frames::EncodeRecords(send, "abc"_ba);
		if (!good || good->size() != 1) {
			qFatal("fuzz_frames: cannot encode");
		}
		const auto forged = input.rest();
		if (forged != good->front()) {
			const auto result = decoder.record(forged);
			if (result.status != Rt::Frames::RecordDecoder::Status::Failed) {
				qFatal("fuzz_frames: a forged record was accepted");
			}
		}
		const auto result = decoder.record(good->front());
		if (result.status != Rt::Frames::RecordDecoder::Status::Complete
			|| result.envelope != "abc"_ba) {
			qFatal("fuzz_frames: the genuine record was lost after a forged one");
		}
	}
	return 0;
}

FUZZ_SEEDS() {
	auto result = QList<QByteArray>();
	auto clear = Rt::Noise::CipherState();
	const auto record = [&](const QByteArray &envelope) {
		auto bytes = QByteArray(1, char(0));
		for (const auto &one : Rt::Frames::EncodeRecords(clear, envelope).value_or(QVector<QByteArray>())) {
			bytes.append(char((one.size() >> 8) & 0xFF));
			bytes.append(char(one.size() & 0xFF));
			bytes.append(one);
		}
		return bytes;
	};
	result.push_back(record("hello"_ba));
	result.push_back(record(QByteArray()));
	result.push_back(record(QByteArray(70000, 'x')));
	// total = 16 MiB - 1, chunk_len above the record, flags without "last".
	result.push_back(QByteArray::fromHex("00" "0040" "01ffff" "ffffff") + QByteArray(58, char(0)));
	result.push_back(QByteArray::fromHex("00" "0040" "00" "0001" "040000") + QByteArray(58, char(0)));
	result.push_back(QByteArray::fromHex("02") + QByteArray(48, char(3)));
	result.push_back(QByteArray::fromHex("03") + QByteArray(80, char(5)));
	return result;
}
