#include "window/window_embeds.h"

#include <QtCore/QHash>

#include <algorithm>

namespace Window {
namespace {

[[nodiscard]] QHash<QString, EmbedFactory> &Embeds() {
	static auto result = QHash<QString, EmbedFactory>();
	return result;
}

[[nodiscard]] QHash<int, QVector<SideSubTab>> &SubTabs() {
	static auto result = QHash<int, QVector<SideSubTab>>();
	return result;
}

[[nodiscard]] QVector<ChatMenuFiller> &MenuFillers() {
	static auto result = QVector<ChatMenuFiller>();
	return result;
}

} // namespace

void RegisterChatMenuFiller(ChatMenuFiller filler) {
	if (filler) {
		MenuFillers().push_back(std::move(filler));
	}
}

bool HasChatMenuFillers() {
	return !MenuFillers().isEmpty();
}

void FillChatMenu(
		Ui::PopupMenu *menu,
		SessionController *controller,
		const QString &chatId) {
	for (const auto &filler : MenuFillers()) {
		filler(menu, controller, chatId);
	}
}

void RegisterEmbed(const QString &id, EmbedFactory factory) {
	Embeds().insert(id, std::move(factory));
}

bool HasEmbed(const QString &id) {
	return Embeds().contains(id);
}

Ui::object_ptr<EmbeddedList> CreateEmbed(
		const QString &id,
		QWidget *parent,
		SessionController *controller,
		const QVariantMap &params) {
	const auto i = Embeds().constFind(id);
	if (i == Embeds().cend() || !(*i)) {
		return Ui::object_ptr<EmbeddedList>(nullptr);
	}
	return (*i)(parent, controller, params);
}

void RegisterSideSubTab(Tab tab, SideSubTab subTab) {
	auto &list = SubTabs()[int(tab)];
	const auto same = std::find_if(list.begin(), list.end(), [&](const SideSubTab &entry) {
		return entry.id == subTab.id;
	});
	if (same != list.end()) {
		*same = std::move(subTab);
	} else {
		list.push_back(std::move(subTab));
	}
	std::stable_sort(list.begin(), list.end(), [](const SideSubTab &a, const SideSubTab &b) {
		return a.order < b.order;
	});
}

QVector<SideSubTab> SideSubTabs(Tab tab) {
	return SubTabs().value(int(tab));
}

} // namespace Window
