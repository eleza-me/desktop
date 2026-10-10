#include "install/install_paths.h"

#include <array>
#include <cstdint>

namespace Install {
namespace {

constexpr auto kMaxComponent = std::size_t(255);
constexpr auto kMaxPath = std::size_t(1024);

[[nodiscard]] char Upper(char ch) {
	return (ch >= 'a' && ch <= 'z') ? char(ch - 'a' + 'A') : ch;
}

[[nodiscard]] bool EqualsNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = std::size_t(0); i != a.size(); ++i) {
		if (Upper(a[i]) != Upper(b[i])) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
	return (text.size() >= prefix.size())
		&& EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

// "NUL", "nul.txt", "COM1.log": the part before the first dot decides.
[[nodiscard]] bool IsDeviceName(std::string_view component) {
	const auto dot = component.find('.');
	auto base = component.substr(0, dot);
	// Windows also ignores trailing spaces of the base name ("NUL .txt").
	while (!base.empty() && base.back() == ' ') {
		base.remove_suffix(1);
	}
	static constexpr auto kNames = std::array<std::string_view, 4>{
		"CON", "PRN", "AUX", "NUL",
	};
	for (const auto name : kNames) {
		if (EqualsNoCase(base, name)) {
			return true;
		}
	}
	if (base.size() == 4
		&& (StartsWithNoCase(base, "COM") || StartsWithNoCase(base, "LPT"))
		&& base[3] >= '0'
		&& base[3] <= '9') {
		return true;
	}
	return EqualsNoCase(base, "CONIN$") || EqualsNoCase(base, "CONOUT$");
}

[[nodiscard]] bool IsWellFormedUtf8(std::string_view text) {
	auto i = std::size_t(0);
	const auto size = text.size();
	while (i < size) {
		const auto lead = static_cast<unsigned char>(text[i]);
		auto length = std::size_t(0);
		auto code = std::uint32_t(0);
		if (lead < 0x80) {
			++i;
			continue;
		} else if ((lead & 0xE0) == 0xC0) {
			length = 2;
			code = lead & 0x1F;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			code = lead & 0x0F;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			code = lead & 0x07;
		} else {
			return false;
		}
		if (i + length > size) {
			return false;
		}
		for (auto k = std::size_t(1); k != length; ++k) {
			const auto next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xC0) != 0x80) {
				return false;
			}
			code = (code << 6) | (next & 0x3F);
		}
		// Overlong forms, surrogates and what is beyond Unicode.
		if ((length == 2 && code < 0x80)
			|| (length == 3 && code < 0x800)
			|| (length == 4 && code < 0x10000)
			|| (code >= 0xD800 && code <= 0xDFFF)
			|| code > 0x10FFFF) {
			return false;
		}
		i += length;
	}
	return true;
}

[[nodiscard]] bool IsSafeComponent(std::string_view component) {
	if (component.empty()
		|| component.size() > kMaxComponent
		|| component == "."
		|| component == ".."
		|| component.back() == '.'
		|| component.back() == ' ') {
		return false;
	}
	for (const auto ch : component) {
		const auto code = static_cast<unsigned char>(ch);
		if (code < 0x20 || code == 0x7F) {
			return false;
		}
		switch (ch) {
		case '<':
		case '>':
		case '"':
		case '|':
		case '?':
		case '*':
		case ':':
		case '\\':
			return false;
		}
	}
	return !IsDeviceName(component);
}

} // namespace

bool IsSafeRelativePath(std::string_view path) {
	if (path.empty()
		|| path.size() > kMaxPath
		|| path.front() == '/'
		|| !IsWellFormedUtf8(path)) {
		return false;
	}
	auto first = true;
	auto from = std::size_t(0);
	while (from <= path.size()) {
		const auto slash = path.find('/', from);
		const auto till = (slash == std::string_view::npos) ? path.size() : slash;
		const auto component = path.substr(from, till - from);
		if (!IsSafeComponent(component)) {
			return false;
		}
		if (first) {
			first = false;
			if (StartsWithNoCase(component, kBackupPrefix)
				|| StartsWithNoCase(component, kIncomingPrefix)
				|| EqualsNoCase(component, kJournalName)) {
				return false;
			}
		}
		if (slash == std::string_view::npos) {
			break;
		}
		from = slash + 1;
	}
	return true;
}

bool IsSafeVersion(std::string_view version) {
	if (version.empty() || version.size() > 32 || version.front() == '.') {
		return false;
	}
	for (const auto ch : version) {
		const auto ok = (ch >= '0' && ch <= '9') || (ch == '.');
		if (!ok) {
			return false;
		}
	}
	return version.back() != '.' && version.find("..") == std::string_view::npos;
}

Path PathFromUtf8(std::string_view text) {
	return Path(std::u8string(
		reinterpret_cast<const char8_t*>(text.data()),
		text.size()));
}

std::string PathToUtf8(const Path &path) {
	const auto text = path.generic_u8string();
	return std::string(
		reinterpret_cast<const char*>(text.data()),
		text.size());
}

} // namespace Install
