// Paragraph direction of a piece of text (UAX #9, rules P2 / P3).
#pragma once

#include <QtCore/QStringView>

#include <optional>

namespace base {

// The direction of the first strong character (L = false, R / AL = true), or
// nullopt when there is none. Characters between an isolate initiator (LRI, RLI,
// FSI) and its matching PDI are skipped, as are the initiators and PDIs themselves:
// an isolated handle or number does not decide the direction of the paragraph.
[[nodiscard]] std::optional<bool> FirstStrongIsRtl(QStringView text);

} // namespace base
