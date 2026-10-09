// The radar: the night cover a speaker sees after opening a topic. An animated
// sweep while waiting for a listener, the matched pair when somebody joined;
// leaves by itself when the conversation ended, expired or was continued.
//
//   controller->showMainSection(Conversation::RadarFactory(id), { .tab = Window::Tab::Chats });
#pragma once

#include "data/data_types.h"
#include "window/window_registry.h"

#include <optional>

namespace Conversation {

// `initial`: a view the caller already has (shown before the first read).
[[nodiscard]] Window::SectionFactory RadarFactory(
	const QString &conversationId,
	std::optional<Data::Conversation> initial = std::nullopt);

} // namespace Conversation
