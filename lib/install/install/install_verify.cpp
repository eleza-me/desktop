#include "install/install_verify.h"

#include "install/install_paths.h"

#include <cstdint>
#include <utility>

namespace Install {
namespace {

constexpr auto kMaxDepth = 32;
constexpr auto kMaxSignedFiles = std::size_t(20000);

// Just enough JSON (RFC 8259) for the manifest: strict, no duplicate keys,
// numbers only as far as integers go.
struct Json {
	enum class Type {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};
	Type type = Type::Null;
	bool boolean = false;
	// Integers up to 2^53 only; anything else is a Number without a value.
	std::optional<std::int64_t> integer;
	std::string string;
	std::vector<Json> array;
	std::vector<std::pair<std::string, Json>> object;

	[[nodiscard]] const Json *find(std::string_view key) const {
		for (const auto &[name, value] : object) {
			if (name == key) {
				return &value;
			}
		}
		return nullptr;
	}
};

class Parser final {
public:
	explicit Parser(std::string_view text) : _text(text) {
	}

	[[nodiscard]] bool parse(Json &result) {
		skipSpace();
		if (!value(result, 0)) {
			return false;
		}
		skipSpace();
		return _position == _text.size();
	}

private:
	void skipSpace() {
		while (_position < _text.size()) {
			const auto c = _text[_position];
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
				break;
			}
			++_position;
		}
	}

	[[nodiscard]] bool literal(std::string_view word) {
		if (_text.substr(_position, word.size()) != word) {
			return false;
		}
		_position += word.size();
		return true;
	}

	[[nodiscard]] bool value(Json &result, int depth) {
		if (depth > kMaxDepth || _position >= _text.size()) {
			return false;
		}
		switch (_text[_position]) {
		case '{': return object(result, depth);
		case '[': return array(result, depth);
		case '"':
			result.type = Json::Type::String;
			return string(result.string);
		case 't':
			result.type = Json::Type::Bool;
			result.boolean = true;
			return literal("true");
		case 'f':
			result.type = Json::Type::Bool;
			return literal("false");
		case 'n':
			return literal("null");
		default:
			return number(result);
		}
	}

	[[nodiscard]] bool object(Json &result, int depth) {
		result.type = Json::Type::Object;
		++_position;
		skipSpace();
		if (_position < _text.size() && _text[_position] == '}') {
			++_position;
			return true;
		}
		for (;;) {
			skipSpace();
			auto key = std::string();
			if (_position >= _text.size()
				|| _text[_position] != '"'
				|| !string(key)
				|| result.find(key)) {
				return false;
			}
			skipSpace();
			if (_position >= _text.size() || _text[_position] != ':') {
				return false;
			}
			++_position;
			skipSpace();
			auto item = Json();
			if (!value(item, depth + 1)) {
				return false;
			}
			result.object.emplace_back(std::move(key), std::move(item));
			skipSpace();
			if (_position >= _text.size()) {
				return false;
			} else if (_text[_position] == ',') {
				++_position;
			} else if (_text[_position] == '}') {
				++_position;
				return true;
			} else {
				return false;
			}
		}
	}

	[[nodiscard]] bool array(Json &result, int depth) {
		result.type = Json::Type::Array;
		++_position;
		skipSpace();
		if (_position < _text.size() && _text[_position] == ']') {
			++_position;
			return true;
		}
		for (;;) {
			skipSpace();
			auto item = Json();
			if (!value(item, depth + 1)) {
				return false;
			}
			result.array.push_back(std::move(item));
			skipSpace();
			if (_position >= _text.size()) {
				return false;
			} else if (_text[_position] == ',') {
				++_position;
			} else if (_text[_position] == ']') {
				++_position;
				return true;
			} else {
				return false;
			}
		}
	}

	[[nodiscard]] bool hex4(std::uint32_t &result) {
		if (_position + 4 > _text.size()) {
			return false;
		}
		result = 0;
		for (auto i = 0; i != 4; ++i) {
			const auto c = _text[_position++];
			result <<= 4;
			if (c >= '0' && c <= '9') {
				result |= std::uint32_t(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				result |= std::uint32_t(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				result |= std::uint32_t(c - 'A' + 10);
			} else {
				return false;
			}
		}
		return true;
	}

	static void appendUtf8(std::string &to, std::uint32_t code) {
		if (code < 0x80) {
			to.push_back(char(code));
		} else if (code < 0x800) {
			to.push_back(char(0xC0 | (code >> 6)));
			to.push_back(char(0x80 | (code & 0x3F)));
		} else if (code < 0x10000) {
			to.push_back(char(0xE0 | (code >> 12)));
			to.push_back(char(0x80 | ((code >> 6) & 0x3F)));
			to.push_back(char(0x80 | (code & 0x3F)));
		} else {
			to.push_back(char(0xF0 | (code >> 18)));
			to.push_back(char(0x80 | ((code >> 12) & 0x3F)));
			to.push_back(char(0x80 | ((code >> 6) & 0x3F)));
			to.push_back(char(0x80 | (code & 0x3F)));
		}
	}

	[[nodiscard]] bool string(std::string &result) {
		++_position; // the opening quote
		result.clear();
		while (_position < _text.size()) {
			const auto c = _text[_position++];
			if (c == '"') {
				return true;
			} else if (static_cast<unsigned char>(c) < 0x20) {
				return false;
			} else if (c != '\\') {
				result.push_back(c);
				continue;
			} else if (_position >= _text.size()) {
				return false;
			}
			switch (_text[_position++]) {
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u': {
				auto code = std::uint32_t();
				if (!hex4(code)) {
					return false;
				}
				if (code >= 0xD800 && code <= 0xDBFF) {
					auto low = std::uint32_t();
					if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF) {
						return false;
					}
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				} else if (code >= 0xDC00 && code <= 0xDFFF) {
					return false;
				}
				appendUtf8(result, code);
			} break;
			default: return false;
			}
		}
		return false;
	}

	[[nodiscard]] bool number(Json &result) {
		result.type = Json::Type::Number;
		const auto start = _position;
		const auto digit = [&] {
			return _position < _text.size()
				&& _text[_position] >= '0'
				&& _text[_position] <= '9';
		};
		const auto negative = (_position < _text.size() && _text[_position] == '-');
		if (negative) {
			++_position;
		}
		if (!digit()) {
			return false;
		}
		auto integral = true;
		if (_text[_position] == '0') {
			++_position;
		} else {
			while (digit()) {
				++_position;
			}
		}
		const auto digitsEnd = _position;
		if (_position < _text.size() && _text[_position] == '.') {
			integral = false;
			++_position;
			if (!digit()) {
				return false;
			}
			while (digit()) {
				++_position;
			}
		}
		if (_position < _text.size()
			&& (_text[_position] == 'e' || _text[_position] == 'E')) {
			integral = false;
			++_position;
			if (_position < _text.size()
				&& (_text[_position] == '+' || _text[_position] == '-')) {
				++_position;
			}
			if (!digit()) {
				return false;
			}
			while (digit()) {
				++_position;
			}
		}
		const auto digits = _text.substr(
			start + (negative ? 1 : 0),
			digitsEnd - start - (negative ? 1 : 0));
		if (integral && digits.size() <= 15) {
			auto value = std::int64_t(0);
			for (const auto c : digits) {
				value = value * 10 + (c - '0');
			}
			result.integer = negative ? -value : value;
		}
		return true;
	}

	std::string_view _text;
	std::size_t _position = 0;

};

[[nodiscard]] bool ParseVersion(std::string_view text, int (&parts)[3]) {
	auto index = 0;
	auto digits = 0;
	parts[0] = parts[1] = parts[2] = 0;
	for (const auto c : text) {
		if (c == '.') {
			if (!digits || ++index > 2) {
				return false;
			}
			digits = 0;
		} else if (c >= '0' && c <= '9' && digits < 5) {
			parts[index] = parts[index] * 10 + (c - '0');
			++digits;
		} else {
			return false;
		}
	}
	return (index == 2) && digits;
}

} // namespace

std::vector<Ed25519PublicKey> DecodePublicKeys(
		const std::vector<std::string_view> &base64) {
	auto result = std::vector<Ed25519PublicKey>();
	for (const auto &text : base64) {
		const auto bytes = DecodeBase64(text);
		if (bytes && bytes->size() == 32) {
			auto key = Ed25519PublicKey();
			for (auto i = std::size_t(0); i != 32; ++i) {
				key[i] = static_cast<std::uint8_t>((*bytes)[i]);
			}
			result.push_back(key);
		}
	}
	return result;
}

std::optional<SignedPackage> ReadSignedPackage(
		std::string_view manifest,
		std::string_view signatureFile,
		const std::vector<Ed25519PublicKey> &keys,
		std::string_view platform,
		std::string &error) {
	const auto fail = [&](const char *reason) {
		error = reason;
		return std::nullopt;
	};
	if (manifest.size() > kMaxManifestBytes
		|| signatureFile.size() > kMaxSignatureBytes) {
		return fail("the manifest is too large");
	}
	const auto signature = DecodeBase64(signatureFile);
	if (!signature || signature->size() != 64) {
		return fail("the signature of the manifest is malformed");
	}
	auto trusted = false;
	for (const auto &key : keys) {
		if (Ed25519Verify(key, manifest, *signature)) {
			trusted = true;
			break;
		}
	}
	if (!trusted) {
		return fail("the manifest is not signed by a trusted key");
	}

	// Only signed bytes from here on.
	auto root = Json();
	if (!Parser(manifest).parse(root) || root.type != Json::Type::Object) {
		return fail("the manifest is not valid JSON");
	}
	const auto format = root.find("format");
	if (!format || format->integer != 1) {
		return fail("the manifest has an unknown format");
	}
	const auto version = root.find("version");
	if (!version
		|| version->type != Json::Type::String
		|| !CompareVersions(version->string, version->string)) {
		return fail("the manifest has no valid version");
	}
	const auto packages = root.find("packages");
	const auto package = (packages && packages->type == Json::Type::Object)
		? packages->find(platform)
		: nullptr;
	const auto files = (package && package->type == Json::Type::Object)
		? package->find("files")
		: nullptr;
	if (!files || files->type != Json::Type::Object || files->object.empty()) {
		return fail("the manifest lists no files for this platform");
	} else if (files->object.size() > kMaxSignedFiles) {
		return fail("the manifest lists too many files");
	}
	auto result = SignedPackage();
	result.version = version->string;
	for (const auto &[relative, hash] : files->object) {
		const auto digest = (hash.type == Json::Type::String)
			? ParseSha256Hex(hash.string)
			: std::nullopt;
		if (!IsSafeRelativePath(relative) || !digest) {
			return fail("the manifest lists a file that is not valid");
		}
		result.files.emplace(relative, *digest);
	}
	return result;
}

std::optional<int> CompareVersions(std::string_view a, std::string_view b) {
	int first[3];
	int second[3];
	if (!ParseVersion(a, first) || !ParseVersion(b, second)) {
		return std::nullopt;
	}
	for (auto i = 0; i != 3; ++i) {
		if (first[i] != second[i]) {
			return (first[i] < second[i]) ? -1 : 1;
		}
	}
	return 0;
}

bool VerifyFiles(
		FileOps &ops,
		const Path &directory,
		const std::vector<std::string> &files,
		const SignedPackage &package,
		std::string &error) {
	auto seen = std::size_t(0);
	for (const auto &relative : files) {
		const auto i = package.files.find(relative);
		if (i == package.files.end()) {
			error = "a file that is not in the signed manifest: " + relative;
			return false;
		}
		++seen;
		auto hasher = Sha256();
		const auto read = ops.readChunks(
			directory / PathFromUtf8(relative),
			[&](const char *data, std::size_t size) {
				hasher.feed(data, size);
				return true;
			});
		if (!read) {
			error = "could not read a new file: " + relative;
			return false;
		} else if (hasher.finish() != i->second) {
			error = "a file does not match the signed manifest: " + relative;
			return false;
		}
	}
	// `files` has no duplicates (a listing of a directory tree).
	if (seen != package.files.size()) {
		error = "files of the signed manifest are missing";
		return false;
	}
	return true;
}

} // namespace Install
