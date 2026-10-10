// The stand-in for libFuzzer (see fuzz_driver.h): seeds + random mutations,
// no coverage feedback. Sanitizers do the detecting.
#include "fuzz_driver.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
// Provided by the sanitizer runtimes; absent in a plain build.
void __sanitizer_set_death_callback(void (*callback)(void)) __attribute__((weak));
int LLVMFuzzerInitialize(int *argc, char ***argv) __attribute__((weak));
}

namespace {

std::vector<uint8_t> Current;
std::string TargetName = "target";

void Save(const char *kind) {
	const auto bytes = QByteArray(
		reinterpret_cast<const char*>(Current.data()),
		qsizetype(Current.size()));
	const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16);
	const auto name = std::string(kind) + "-" + TargetName + "-" + hash.constData();
	if (const auto file = std::fopen(name.c_str(), "wb")) {
		std::fwrite(Current.data(), 1, Current.size(), file);
		std::fclose(file);
		std::fprintf(stderr, "\n[fuzz] input saved to ./%s (%zu bytes)\n", name.c_str(), Current.size());
	}
}

void Died() {
	Save("crash");
}

using Clock = std::chrono::steady_clock;

[[nodiscard]] double Seconds(Clock::time_point from) {
	return std::chrono::duration<double>(Clock::now() - from).count();
}

const uint8_t kInteresting[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x08, 0x10, 0x12, 0x1A, 0x22, 0x0A, 0x20 };

void Mutate(std::vector<uint8_t> &data, std::mt19937_64 &random, size_t maxLength, const std::vector<std::vector<uint8_t>> &corpus) {
	const auto pick = [&](size_t limit) {
		return limit ? size_t(random() % limit) : size_t(0);
	};
	const auto rounds = 1 + pick(6);
	for (auto round = size_t(0); round != rounds; ++round) {
		switch (pick(12)) {
		case 0: // flip a bit
			if (!data.empty()) {
				data[pick(data.size())] ^= uint8_t(1U << pick(8));
			}
			break;
		case 1: // set a byte
			if (!data.empty()) {
				data[pick(data.size())] = uint8_t(random());
			}
			break;
		case 2: // an interesting byte
			if (!data.empty()) {
				data[pick(data.size())] = kInteresting[pick(sizeof(kInteresting))];
			}
			break;
		case 3: // insert random bytes
			if (data.size() < maxLength) {
				const auto count = 1 + pick(8);
				const auto at = pick(data.size() + 1);
				for (auto i = size_t(0); i != count; ++i) {
					data.insert(data.begin() + at, uint8_t(random()));
				}
			}
			break;
		case 4: // erase a range
			if (data.size() > 1) {
				const auto at = pick(data.size());
				const auto count = 1 + pick(std::min<size_t>(data.size() - at, 16));
				data.erase(data.begin() + at, data.begin() + at + count);
			}
			break;
		case 5: // duplicate a range
			if (!data.empty() && data.size() < maxLength) {
				const auto at = pick(data.size());
				const auto count = 1 + pick(std::min<size_t>(data.size() - at, 64));
				const auto copy = std::vector<uint8_t>(data.begin() + at, data.begin() + at + count);
				const auto to = pick(data.size() + 1);
				data.insert(data.begin() + to, copy.begin(), copy.end());
			}
			break;
		case 6: // a varint-like run of continuation bytes
			if (data.size() < maxLength) {
				const auto at = pick(data.size() + 1);
				const auto count = 1 + pick(11);
				data.insert(data.begin() + at, count, uint8_t(0x80 | pick(128)));
			}
			break;
		case 7: // a big-endian / little-endian extreme
			if (data.size() >= 4) {
				const auto at = pick(data.size() - 3);
				const uint32_t values[] = { 0U, 1U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFFU, 0x00010000U, 0x00FFFFFFU };
				auto value = values[pick(7)];
				std::memcpy(data.data() + at, &value, 4);
			}
			break;
		case 8: // splice with another corpus entry
			if (!corpus.empty()) {
				const auto &other = corpus[pick(corpus.size())];
				if (!other.empty()) {
					const auto from = pick(other.size());
					const auto count = 1 + pick(other.size() - from);
					const auto at = pick(data.size() + 1);
					data.insert(data.begin() + at, other.begin() + from, other.begin() + from + count);
				}
			}
			break;
		case 9: // truncate
			if (data.size() > 1) {
				data.resize(1 + pick(data.size() - 1));
			}
			break;
		case 10: // repeat one byte many times
			if (data.size() < maxLength) {
				const auto at = pick(data.size() + 1);
				const auto count = 1 + pick(512);
				data.insert(data.begin() + at, count, data.empty() ? uint8_t(random()) : data[pick(data.size())]);
			}
			break;
		default: // swap two bytes
			if (data.size() > 1) {
				std::swap(data[pick(data.size())], data[pick(data.size())]);
			}
			break;
		}
	}
	if (data.size() > maxLength) {
		data.resize(maxLength);
	}
}

} // namespace

__attribute__((weak)) QList<QByteArray> Fuzz::Seeds() {
	return {};
}

int main(int argc, char *argv[]) {
	if (argv[0]) {
		const auto slash = std::strrchr(argv[0], '/');
		TargetName = slash ? (slash + 1) : argv[0];
	}
	auto totalTime = 60.;
	auto timeout = 10.;
	auto maxLength = size_t(1) << 16;
	auto seed = uint64_t(std::random_device()());
	auto runs = uint64_t(0);
	auto files = std::vector<std::string>();
	for (auto i = 1; i < argc; ++i) {
		const auto argument = std::string(argv[i]);
		const auto value = [&](const char *name) -> const char* {
			const auto prefix = std::string(name) + "=";
			return (argument.compare(0, prefix.size(), prefix) == 0)
				? (argv[i] + prefix.size())
				: nullptr;
		};
		if (const auto v = value("-max_total_time")) {
			totalTime = std::atof(v);
		} else if (const auto v = value("-timeout")) {
			timeout = std::atof(v);
		} else if (const auto v = value("-max_len")) {
			maxLength = size_t(std::atoll(v));
		} else if (const auto v = value("-seed")) {
			seed = uint64_t(std::atoll(v));
		} else if (const auto v = value("-runs")) {
			runs = uint64_t(std::atoll(v));
		} else if (!argument.empty() && argument[0] != '-') {
			files.push_back(argument);
		}
	}
	if (__sanitizer_set_death_callback) {
		__sanitizer_set_death_callback(&Died);
	}
	if (LLVMFuzzerInitialize) {
		LLVMFuzzerInitialize(&argc, &argv);
	}
	const auto runOne = [&](const std::vector<uint8_t> &input) {
		Current = input;
		// A copy without spare capacity: an overread is seen by ASan.
		const auto copy = std::vector<uint8_t>(input.begin(), input.end());
		const auto started = Clock::now();
		LLVMFuzzerTestOneInput(copy.data(), copy.size());
		const auto took = Seconds(started);
		if (took > timeout) {
			std::fprintf(stderr, "[fuzz] SLOW: one input took %.1f s (limit %.1f s)\n", took, timeout);
			Save("slow");
			std::abort();
		}
		return took;
	};
	if (!files.empty()) {
		for (const auto &name : files) {
			auto file = QFile(QString::fromLocal8Bit(name.c_str()));
			if (!file.open(QIODevice::ReadOnly)) {
				std::fprintf(stderr, "[fuzz] cannot read %s\n", name.c_str());
				return 2;
			}
			const auto bytes = file.readAll();
			const auto took = runOne(std::vector<uint8_t>(bytes.begin(), bytes.end()));
			std::fprintf(stderr, "[fuzz] %s: ok (%.3f s)\n", name.c_str(), took);
		}
		return 0;
	}
	auto corpus = std::vector<std::vector<uint8_t>>();
	corpus.push_back({});
	for (const auto &entry : Fuzz::Seeds()) {
		corpus.emplace_back(entry.begin(), entry.end());
	}
	auto random = std::mt19937_64(seed);
	std::fprintf(stderr, "[fuzz] %s: seed %llu, %zu corpus entries, %.0f s\n",
		TargetName.c_str(), static_cast<unsigned long long>(seed), corpus.size(), totalTime);
	const auto started = Clock::now();
	auto executed = uint64_t(0);
	auto slowest = 0.;
	for (const auto &entry : corpus) {
		slowest = std::max(slowest, runOne(entry));
		++executed;
	}
	const auto seeds = corpus.size();
	while ((runs == 0 || executed < runs) && Seconds(started) < totalTime) {
		auto input = corpus[size_t(random() % corpus.size())];
		Mutate(input, random, maxLength, corpus);
		slowest = std::max(slowest, runOne(input));
		++executed;
		// Without coverage every survivor is as good as another: keep a few
		// so mutations stack up, replacing old ones (never the seeds).
		if (random() % 16 == 0) {
			if (corpus.size() < seeds + 4096) {
				corpus.push_back(std::move(input));
			} else {
				corpus[seeds + size_t(random() % (corpus.size() - seeds))] = std::move(input);
			}
		}
	}
	std::fprintf(stderr, "[fuzz] %s: %llu executions in %.0f s, slowest %.3f s, no failure\n",
		TargetName.c_str(), static_cast<unsigned long long>(executed), Seconds(started), slowest);
	return 0;
}
