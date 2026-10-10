// Decodes a blurhash (the compact placeholder string that comes with photos
// and videos) into a tiny image to show while the real one loads.
#pragma once

#include "base/basic_types.h"

#include <QtGui/QImage>

namespace Data {

// A null image when the hash is malformed. 32 x 32 is plenty: scale it up
// with smooth transformation when painting.
[[nodiscard]] QImage BlurhashImage(const QString &hash, int width = 32, int height = 32);

} // namespace Data
