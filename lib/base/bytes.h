// Byte string helpers: hex, base64 (standard and URL-safe), concatenation,
// big-endian integers, wiping secrets.
#pragma once

#include "base/basic_types.h"

#include <initializer_list>

namespace base::bytes {

[[nodiscard]] QByteArray concat(std::initializer_list<QByteArray> parts);

[[nodiscard]] QString toHex(const QByteArray &bytes);
// Returns a null array when `hex` has an odd length or a non-hex character.
[[nodiscard]] QByteArray fromHex(const QString &hex);

[[nodiscard]] QString toBase64(const QByteArray &bytes);
[[nodiscard]] QString toBase64Url(const QByteArray &bytes); // no padding
// Accepts standard and URL-safe alphabets, with or without padding; other
// characters (whitespace) are ignored, as the web client does.
[[nodiscard]] QByteArray fromBase64(const QString &text);

// Overwrites the array's storage with zeros and clears it.
void wipe(QByteArray &bytes);

// Comparison whose duration does not depend on where the arrays differ.
[[nodiscard]] bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

} // namespace base::bytes
