#include "install/install_crypto.h"

#include <algorithm>
#include <cstring>

namespace Install {
namespace {

constexpr std::uint32_t kSha256K[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

constexpr std::uint64_t kSha512K[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

[[nodiscard]] constexpr std::uint32_t Rotr32(std::uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

[[nodiscard]] constexpr std::uint64_t Rotr64(std::uint64_t x, int n) {
	return (x >> n) | (x << (64 - n));
}

// ---- Ed25519 verification, after TweetNaCl (public domain): field
// elements of GF(2^255 - 19) as 16 limbs of 16 bits, points in extended
// coordinates.

using Gf = std::int64_t[16];

constexpr Gf kGf0 = {};
constexpr Gf kGf1 = { 1 };
// d = -121665 / 121666, 2d, the base point (x, y) and sqrt(-1).
constexpr Gf kD = {
	0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
	0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
constexpr Gf kD2 = {
	0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
	0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
constexpr Gf kX = {
	0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
	0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
constexpr Gf kY = {
	0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
constexpr Gf kI = {
	0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
	0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};
// The group order L, little endian.
constexpr std::int64_t kL[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0x10,
};

void Set(Gf r, const Gf a) {
	for (auto i = 0; i != 16; ++i) {
		r[i] = a[i];
	}
}

void Carry(Gf o) {
	for (auto i = 0; i != 16; ++i) {
		o[i] += (std::int64_t(1) << 16);
		const auto c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * (std::int64_t(1) << 16);
	}
}

void Select(Gf p, Gf q, int b) {
	const auto c = ~(std::int64_t(b) - 1);
	for (auto i = 0; i != 16; ++i) {
		const auto t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

void Pack25519(std::uint8_t *o, const Gf n) {
	Gf m = {};
	Gf t = {};
	Set(t, n);
	Carry(t);
	Carry(t);
	Carry(t);
	for (auto j = 0; j != 2; ++j) {
		m[0] = t[0] - 0xffed;
		for (auto i = 1; i != 15; ++i) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		const auto b = int((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		Select(t, m, 1 - b);
	}
	for (auto i = 0; i != 16; ++i) {
		o[2 * i] = std::uint8_t(t[i] & 0xff);
		o[2 * i + 1] = std::uint8_t((t[i] >> 8) & 0xff);
	}
}

[[nodiscard]] bool Equal25519(const Gf a, const Gf b) {
	std::uint8_t c[32];
	std::uint8_t d[32];
	Pack25519(c, a);
	Pack25519(d, b);
	return std::memcmp(c, d, 32) == 0;
}

[[nodiscard]] std::uint8_t Parity(const Gf a) {
	std::uint8_t d[32];
	Pack25519(d, a);
	return d[0] & 1;
}

void Unpack25519(Gf o, const std::uint8_t *n) {
	for (auto i = 0; i != 16; ++i) {
		o[i] = n[2 * i] + (std::int64_t(n[2 * i + 1]) << 8);
	}
	o[15] &= 0x7fff;
}

void Add(Gf o, const Gf a, const Gf b) {
	for (auto i = 0; i != 16; ++i) {
		o[i] = a[i] + b[i];
	}
}

void Sub(Gf o, const Gf a, const Gf b) {
	for (auto i = 0; i != 16; ++i) {
		o[i] = a[i] - b[i];
	}
}

void Mul(Gf o, const Gf a, const Gf b) {
	std::int64_t t[31] = {};
	for (auto i = 0; i != 16; ++i) {
		for (auto j = 0; j != 16; ++j) {
			t[i + j] += a[i] * b[j];
		}
	}
	for (auto i = 0; i != 15; ++i) {
		t[i] += 38 * t[i + 16];
	}
	for (auto i = 0; i != 16; ++i) {
		o[i] = t[i];
	}
	Carry(o);
	Carry(o);
}

void Square(Gf o, const Gf a) {
	Mul(o, a, a);
}

// a^(2^252 - 3), for the square root.
void Pow2523(Gf o, const Gf i) {
	Gf c = {};
	Set(c, i);
	for (auto a = 250; a >= 0; --a) {
		Square(c, c);
		if (a != 1) {
			Mul(c, c, i);
		}
	}
	Set(o, c);
}

void Invert(Gf o, const Gf i) {
	Gf c = {};
	Set(c, i);
	for (auto a = 253; a >= 0; --a) {
		Square(c, c);
		if (a != 2 && a != 4) {
			Mul(c, c, i);
		}
	}
	Set(o, c);
}

using Point = Gf[4];

void PointAdd(Point p, Point q) {
	Gf a = {}, b = {}, c = {}, d = {}, t = {}, e = {}, f = {}, g = {}, h = {};
	Sub(a, p[1], p[0]);
	Sub(t, q[1], q[0]);
	Mul(a, a, t);
	Add(b, p[0], p[1]);
	Add(t, q[0], q[1]);
	Mul(b, b, t);
	Mul(c, p[3], q[3]);
	Mul(c, c, kD2);
	Mul(d, p[2], q[2]);
	Add(d, d, d);
	Sub(e, b, a);
	Sub(f, d, c);
	Add(g, d, c);
	Add(h, b, a);
	Mul(p[0], e, f);
	Mul(p[1], h, g);
	Mul(p[2], g, f);
	Mul(p[3], e, h);
}

void PointSwap(Point p, Point q, std::uint8_t b) {
	for (auto i = 0; i != 4; ++i) {
		Select(p[i], q[i], b);
	}
}

void PointPack(std::uint8_t *r, Point p) {
	Gf tx = {}, ty = {}, zi = {};
	Invert(zi, p[2]);
	Mul(tx, p[0], zi);
	Mul(ty, p[1], zi);
	Pack25519(r, ty);
	r[31] ^= std::uint8_t(Parity(tx) << 7);
}

void ScalarMult(Point p, Point q, const std::uint8_t *s) {
	Set(p[0], kGf0);
	Set(p[1], kGf1);
	Set(p[2], kGf1);
	Set(p[3], kGf0);
	for (auto i = 255; i >= 0; --i) {
		const auto b = std::uint8_t((s[i / 8] >> (i & 7)) & 1);
		PointSwap(p, q, b);
		PointAdd(q, p);
		PointAdd(p, p);
		PointSwap(p, q, b);
	}
}

void ScalarBase(Point p, const std::uint8_t *s) {
	Point q = {};
	Set(q[0], kX);
	Set(q[1], kY);
	Set(q[2], kGf1);
	Mul(q[3], kX, kY);
	ScalarMult(p, q, s);
}

// r = x mod L, x being 64 little-endian "digits" (bytes, possibly carried).
void ModL(std::uint8_t *r, std::int64_t x[64]) {
	for (auto i = 63; i >= 32; --i) {
		auto carry = std::int64_t(0);
		auto j = i - 32;
		for (; j < i - 12; ++j) {
			x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry * 256;
		}
		x[j] += carry;
		x[i] = 0;
	}
	auto carry = std::int64_t(0);
	for (auto j = 0; j != 32; ++j) {
		x[j] += carry - (x[31] >> 4) * kL[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (auto j = 0; j != 32; ++j) {
		x[j] -= carry * kL[j];
	}
	for (auto i = 0; i != 32; ++i) {
		x[i + 1] += x[i] >> 8;
		r[i] = std::uint8_t(x[i] & 255);
	}
}

void Reduce(std::uint8_t *r) {
	std::int64_t x[64] = {};
	for (auto i = 0; i != 64; ++i) {
		x[i] = r[i];
		r[i] = 0;
	}
	ModL(r, x);
}

// -A from its encoding; false when it is not a point.
[[nodiscard]] bool UnpackNegative(Point r, const std::uint8_t *p) {
	Gf t = {}, check = {}, num = {}, den = {}, den2 = {}, den4 = {}, den6 = {};
	Set(r[2], kGf1);
	Unpack25519(r[1], p);
	Square(num, r[1]);
	Mul(den, num, kD);
	Sub(num, num, r[2]);
	Add(den, r[2], den);

	Square(den2, den);
	Square(den4, den2);
	Mul(den6, den4, den2);
	Mul(t, den6, num);
	Mul(t, t, den);

	Pow2523(t, t);
	Mul(t, t, num);
	Mul(t, t, den);
	Mul(t, t, den);
	Mul(r[0], t, den);

	Square(check, r[0]);
	Mul(check, check, den);
	if (!Equal25519(check, num)) {
		Mul(r[0], r[0], kI);
	}
	Square(check, r[0]);
	Mul(check, check, den);
	if (!Equal25519(check, num)) {
		return false;
	}
	if (Parity(r[0]) == (p[31] >> 7)) {
		Sub(r[0], kGf0, r[0]);
	}
	Mul(r[3], r[0], r[1]);
	return true;
}

// S < L (little endian).
[[nodiscard]] bool IsCanonicalScalar(const std::uint8_t *s) {
	for (auto i = 31; i >= 0; --i) {
		if (s[i] != kL[i]) {
			return s[i] < kL[i];
		}
	}
	return false; // S == L
}

[[nodiscard]] int Base64Value(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	} else if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	} else if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	} else if (c == '+') {
		return 62;
	} else if (c == '/') {
		return 63;
	}
	return -1;
}

[[nodiscard]] int HexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	} else if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	} else if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

} // namespace

Sha256::Sha256()
: _state({
	0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
	0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
}) {
}

void Sha256::block(const std::uint8_t *data) {
	std::uint32_t w[64];
	for (auto i = 0; i != 16; ++i) {
		w[i] = (std::uint32_t(data[4 * i]) << 24)
			| (std::uint32_t(data[4 * i + 1]) << 16)
			| (std::uint32_t(data[4 * i + 2]) << 8)
			| std::uint32_t(data[4 * i + 3]);
	}
	for (auto i = 16; i != 64; ++i) {
		const auto s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const auto s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto a = _state[0], b = _state[1], c = _state[2], d = _state[3];
	auto e = _state[4], f = _state[5], g = _state[6], h = _state[7];
	for (auto i = 0; i != 64; ++i) {
		const auto s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
		const auto ch = (e & f) ^ (~e & g);
		const auto t1 = h + s1 + ch + kSha256K[i] + w[i];
		const auto s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
		const auto maj = (a & b) ^ (a & c) ^ (b & c);
		const auto t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	_state[0] += a;
	_state[1] += b;
	_state[2] += c;
	_state[3] += d;
	_state[4] += e;
	_state[5] += f;
	_state[6] += g;
	_state[7] += h;
}

void Sha256::feed(const void *data, std::size_t size) {
	auto bytes = static_cast<const std::uint8_t*>(data);
	_length += size;
	while (size > 0) {
		const auto part = std::min(size, _buffer.size() - _buffered);
		std::memcpy(_buffer.data() + _buffered, bytes, part);
		_buffered += part;
		bytes += part;
		size -= part;
		if (_buffered == _buffer.size()) {
			block(_buffer.data());
			_buffered = 0;
		}
	}
}

Sha256Digest Sha256::finish() {
	const auto bits = _length * 8;
	const std::uint8_t one = 0x80;
	feed(&one, 1);
	const std::uint8_t zero = 0;
	while (_buffered != 56) {
		feed(&zero, 1);
	}
	std::uint8_t length[8];
	for (auto i = 0; i != 8; ++i) {
		length[i] = std::uint8_t(bits >> (56 - 8 * i));
	}
	feed(length, 8);
	auto result = Sha256Digest();
	for (auto i = 0; i != 8; ++i) {
		for (auto j = 0; j != 4; ++j) {
			result[4 * i + j] = std::uint8_t(_state[i] >> (24 - 8 * j));
		}
	}
	return result;
}

Sha256Digest Sha256::Of(std::string_view data) {
	auto hasher = Sha256();
	hasher.feed(data.data(), data.size());
	return hasher.finish();
}

Sha512::Sha512()
: _state({
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
}) {
}

void Sha512::block(const std::uint8_t *data) {
	std::uint64_t w[80];
	for (auto i = 0; i != 16; ++i) {
		w[i] = 0;
		for (auto j = 0; j != 8; ++j) {
			w[i] = (w[i] << 8) | data[8 * i + j];
		}
	}
	for (auto i = 16; i != 80; ++i) {
		const auto s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		const auto s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto a = _state[0], b = _state[1], c = _state[2], d = _state[3];
	auto e = _state[4], f = _state[5], g = _state[6], h = _state[7];
	for (auto i = 0; i != 80; ++i) {
		const auto s1 = Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41);
		const auto ch = (e & f) ^ (~e & g);
		const auto t1 = h + s1 + ch + kSha512K[i] + w[i];
		const auto s0 = Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39);
		const auto maj = (a & b) ^ (a & c) ^ (b & c);
		const auto t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	_state[0] += a;
	_state[1] += b;
	_state[2] += c;
	_state[3] += d;
	_state[4] += e;
	_state[5] += f;
	_state[6] += g;
	_state[7] += h;
}

void Sha512::feed(const void *data, std::size_t size) {
	auto bytes = static_cast<const std::uint8_t*>(data);
	_length += size;
	while (size > 0) {
		const auto part = std::min(size, _buffer.size() - _buffered);
		std::memcpy(_buffer.data() + _buffered, bytes, part);
		_buffered += part;
		bytes += part;
		size -= part;
		if (_buffered == _buffer.size()) {
			block(_buffer.data());
			_buffered = 0;
		}
	}
}

Sha512Digest Sha512::finish() {
	// The length is a 128-bit number; messages here are far below 2^61 bytes.
	const auto bits = _length * 8;
	const std::uint8_t one = 0x80;
	feed(&one, 1);
	const std::uint8_t zero = 0;
	while (_buffered != 112) {
		feed(&zero, 1);
	}
	std::uint8_t length[16] = {};
	for (auto i = 0; i != 8; ++i) {
		length[8 + i] = std::uint8_t(bits >> (56 - 8 * i));
	}
	feed(length, 16);
	auto result = Sha512Digest();
	for (auto i = 0; i != 8; ++i) {
		for (auto j = 0; j != 8; ++j) {
			result[8 * i + j] = std::uint8_t(_state[i] >> (56 - 8 * j));
		}
	}
	return result;
}

bool Ed25519Verify(
		const Ed25519PublicKey &publicKey,
		std::string_view message,
		std::string_view signature) {
	if (signature.size() != 64) {
		return false;
	}
	const auto sig = reinterpret_cast<const std::uint8_t*>(signature.data());
	if (!IsCanonicalScalar(sig + 32)) {
		return false;
	}
	Point q = {};
	if (!UnpackNegative(q, publicKey.data())) {
		return false;
	}
	// h = SHA-512(R || A || M) mod L
	auto hasher = Sha512();
	hasher.feed(sig, 32);
	hasher.feed(publicKey.data(), publicKey.size());
	hasher.feed(message.data(), message.size());
	auto h = hasher.finish();
	Reduce(h.data());

	// [S]B - [h]A must be R.
	Point p = {};
	ScalarMult(p, q, h.data());
	ScalarBase(q, sig + 32);
	PointAdd(p, q);
	std::uint8_t t[32];
	PointPack(t, p);
	return std::memcmp(t, sig, 32) == 0;
}

std::optional<std::string> DecodeBase64(std::string_view text) {
	const auto space = [](char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	while (!text.empty() && space(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && space(text.back())) {
		text.remove_suffix(1);
	}
	if (text.empty() || text.size() % 4 != 0) {
		return std::nullopt;
	}
	auto padding = std::size_t(0);
	while (padding < 2 && text[text.size() - 1 - padding] == '=') {
		++padding;
	}
	auto result = std::string();
	result.reserve(text.size() / 4 * 3);
	auto accumulator = std::uint32_t(0);
	for (auto i = std::size_t(0); i != text.size(); ++i) {
		const auto value = (i >= text.size() - padding)
			? 0
			: Base64Value(text[i]);
		if (value < 0) {
			return std::nullopt;
		}
		accumulator = (accumulator << 6) | std::uint32_t(value);
		if (i % 4 == 3) {
			result.push_back(char((accumulator >> 16) & 0xff));
			result.push_back(char((accumulator >> 8) & 0xff));
			result.push_back(char(accumulator & 0xff));
			accumulator = 0;
		}
	}
	result.resize(result.size() - padding);
	return result;
}

std::optional<Sha256Digest> ParseSha256Hex(std::string_view text) {
	if (text.size() != 64) {
		return std::nullopt;
	}
	auto result = Sha256Digest();
	for (auto i = std::size_t(0); i != 32; ++i) {
		const auto high = HexValue(text[2 * i]);
		const auto low = HexValue(text[2 * i + 1]);
		if (high < 0 || low < 0) {
			return std::nullopt;
		}
		result[i] = std::uint8_t((high << 4) | low);
	}
	return result;
}

} // namespace Install
