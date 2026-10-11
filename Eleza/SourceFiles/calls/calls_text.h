// How a call reads: the line under the name («در حال زنگ زدن…», «۰۲:۱۵»,
// «رد شد»), the notices, the name of the other side.
#pragma once

#include "calls/calls_instance.h"

namespace Main {
class Session;
} // namespace Main

namespace Calls {

// «۰۲:۱۵», «۱:۰۲:۱۵» with the digits of the interface language.
[[nodiscard]] QString ClockText(qint64 seconds);

// The state line of a live call. In the call it is the time since both
// sides were connected (`nowMs`: the clock, ms since the epoch).
[[nodiscard]] QString StateText(const Call &call, qint64 nowMs);
// How an ended call ended.
[[nodiscard]] QString EndedText(const Call &call);
// Whether the state line is a warning (reconnecting, a failed call).
[[nodiscard]] bool StateIsWarning(const Call &call);
// «تماس دوباره» makes sense after this end (I called and it did not happen).
[[nodiscard]] bool CanCallAgain(const Call &call);

[[nodiscard]] QString NoticeText(Notice notice);

// The display name of a user as the chat list shows it ("" until the card
// is here: Data::Users::changed follows).
[[nodiscard]] QString PeerName(Main::Session *session, const QString &userId);

} // namespace Calls
