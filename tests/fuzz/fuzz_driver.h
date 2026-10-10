// Shared by every fuzz target (tests/fuzz).
//
// A target is one .cpp with the libFuzzer entry point:
//
//   extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
//
// and, optionally, seeds for the built-in driver:
//
//   FUZZ_SEEDS() { return { QByteArray("..."), ... }; }
//
// With clang and its libFuzzer runtime the targets are real libFuzzer
// binaries (ELEZA_FUZZ_ENGINE=libfuzzer). Elsewhere fuzz_driver.cpp provides
// main(): a small coverage-blind mutation loop over the seeds, with the same
// command line where it matters:
//
//   fuzz_proto -max_total_time=300 [-seed=N] [-max_len=N] [-timeout=S] [file...]
//
// A file argument is run once (to reproduce a saved crash). When a sanitizer
// aborts, or one input takes longer than -timeout seconds, the input is
// written to ./crash-<target>-<hash> (or slow-...) first.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace Fuzz {

// Defined by the target (FUZZ_SEEDS) or by the weak default: no seeds.
QList<QByteArray> Seeds();

// A cursor over the input for targets that build structured values from it.
class Input {
public:
	Input(const uint8_t *data, size_t size) : _data(data), _size(size) {
	}

	[[nodiscard]] bool empty() const {
		return _position >= _size;
	}
	[[nodiscard]] size_t left() const {
		return _size - _position;
	}
	[[nodiscard]] uint8_t byte() {
		return empty() ? uint8_t(0) : _data[_position++];
	}
	[[nodiscard]] uint32_t u32() {
		auto result = uint32_t(0);
		for (auto i = 0; i != 4; ++i) {
			result = (result << 8) | byte();
		}
		return result;
	}
	[[nodiscard]] uint64_t u64() {
		return (uint64_t(u32()) << 32) | u32();
	}
	// 0 .. limit - 1.
	[[nodiscard]] uint32_t below(uint32_t limit) {
		return limit ? (u32() % limit) : 0;
	}
	[[nodiscard]] QByteArray bytes(size_t count) {
		const auto take = (count < left()) ? count : left();
		const auto result = QByteArray(
			reinterpret_cast<const char*>(_data + _position),
			qsizetype(take));
		_position += take;
		return result;
	}
	[[nodiscard]] QByteArray rest() {
		return bytes(left());
	}

private:
	const uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _position = 0;

};

} // namespace Fuzz

#define FUZZ_SEEDS() QList<QByteArray> Fuzz::Seeds()
