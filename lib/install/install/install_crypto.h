// The few checks the elevated Windows updater makes on its own: SHA-256 of
// the staged files, and Ed25519 (RFC 8032) over the signed update manifest,
// with SHA-512 under it. Plain C++ without any library: the updater links
// nothing but the C++ runtime and must not load a DLL of the app (OpenSSL
// among them). Only verification, on public data: nothing here needs to run
// in constant time. The tests check it against OpenSSL.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Install {

using Sha256Digest = std::array<std::uint8_t, 32>;
using Sha512Digest = std::array<std::uint8_t, 64>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;

class Sha256 {
public:
	Sha256();

	void feed(const void *data, std::size_t size);
	[[nodiscard]] Sha256Digest finish();

	[[nodiscard]] static Sha256Digest Of(std::string_view data);

private:
	void block(const std::uint8_t *data);

	std::array<std::uint32_t, 8> _state = {};
	std::array<std::uint8_t, 64> _buffer = {};
	std::size_t _buffered = 0;
	std::uint64_t _length = 0;

};

class Sha512 {
public:
	Sha512();

	void feed(const void *data, std::size_t size);
	[[nodiscard]] Sha512Digest finish();

private:
	void block(const std::uint8_t *data);

	std::array<std::uint64_t, 8> _state = {};
	std::array<std::uint8_t, 128> _buffer = {};
	std::size_t _buffered = 0;
	std::uint64_t _length = 0;

};

// True when `signature` (64 bytes) is a valid signature of `message` by
// `publicKey`. Non-canonical signatures (S >= L) are refused, like OpenSSL
// does.
[[nodiscard]] bool Ed25519Verify(
	const Ed25519PublicKey &publicKey,
	std::string_view message,
	std::string_view signature);

// Standard base64 (with padding); whitespace around it is ignored. nullopt
// for anything else.
[[nodiscard]] std::optional<std::string> DecodeBase64(std::string_view text);

// 64 hex digits (either case) -> 32 bytes.
[[nodiscard]] std::optional<Sha256Digest> ParseSha256Hex(std::string_view text);

} // namespace Install
