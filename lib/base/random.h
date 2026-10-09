// Non-cryptographic conveniences over the system RNG. Key material comes
// from Crypto::RandomBytes (OpenSSL), not from here.
#pragma once

#include "base/basic_types.h"

namespace base {

// `size` bytes from the operating system's generator.
[[nodiscard]] QByteArray RandomBytes(int size);

// A lowercase RFC 4122 v4 UUID without braces ("3f2c...-...").
[[nodiscard]] QString RandomUuid();

// Uniform in [0, 1).
[[nodiscard]] double RandomUnit();

// Uniform in [0, max).
[[nodiscard]] quint32 RandomBounded(quint32 max);

} // namespace base
