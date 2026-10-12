// «پیام جدید»: pick a person and open (or create) the direct chat.
#include "dialogs/dialogs_menu.h"

#include "api/api_auth.h"
#include "boxes/peer_list_box.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

#include <QtCore/QSet>

namespace Dialogs {
namespace {

constexpr auto kRecentLimit = 20;
const auto kSavedRowId = u"saved"_s;
const auto kNewGroupRowId = u"new_group"_s;

class NewMessageController final : public Boxes::PeerListController {
public:
	explicit NewMessageController(Window::SessionController *controller)
	: _controller(controller) {
	}

	QString title() const override {
		return tr::lng_dialogs_new_message(tr::now);
	}
	QString searchPlaceholder() const override {
		return tr::lng_dialogs_new_message_search(tr::now);
	}
	QString emptyText() const override {
		return tr::lng_nothing_found(tr::now);
	}
	bool searchRemotely() const override {
		return true;
	}

	void prepare() override {
		const auto session = this->session();
		auto saved = Boxes::PeerListRow();
		saved.id = kSavedRowId;
		saved.title = tr::lng_saved_messages(tr::now);
		saved.special = Ui::UserpicKind::Saved;
		if (session->data().groups().enabled()) {
			// «گروه تازه» leads the list while the server has groups.
			auto group = Boxes::PeerListRow();
			group.id = kNewGroupRowId;
			group.title = tr::lng_groups_new_group(tr::now);
			group.special = Ui::UserpicKind::User;
			_base.push_back(group);
		}
		_base.push_back(saved);

		// The people of the recent chats…
		auto count = 0;
		for (const auto &chat : session->data().chats().list(
				Data::Chats::Section::Messages)) {
			if (chat.saved
				|| chat.anonymous
				|| chat.peerDeleted
				|| chat.peerUserId.isEmpty()) {
				continue;
			}
			auto user = session->data().users().get(chat.peerUserId);
			if (!user.valid()) {
				// The card is on its way; the chat knows the name.
				user.id = chat.peerUserId;
				user.displayName = chat.title;
				user.verified = chat.verified;
			}
			add(user, chat.title);
			if (++count >= kRecentLimit) {
				break;
			}
		}
		delegate()->setRows(_base);

		// …then the ones I follow.
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().graph().following(
			session->userId(),
			QString(),
			guard,
			[this](const Data::GraphList &list) {
				for (const auto &profile : list.users) {
					if (!profile.deleted && !profile.me) {
						add(profile, profile.displayName);
					}
				}
				if (delegate()->searchQuery().trimmed().isEmpty()) {
					delegate()->setRows(_base);
				}
			},
			[](const Api::Error &) {
				// The recent people are enough to start with.
			});
	}

	void searchQueryChanged(const QString &query) override {
		const auto request = ++_request;
		auto text = query.trimmed();
		if (text.isEmpty()) {
			delegate()->setRows(_base);
			return;
		}
		while (text.startsWith(QLatin1Char('@'))) {
			text.remove(0, 1);
		}
		if (text.isEmpty()) {
			delegate()->setRows({});
			return;
		}
		delegate()->setLoading(true);
		const auto session = this->session();
		const auto guard = base::Guard(delegate()->peerListBox());
		const auto handle = text.toLower();
		session->data().graph().searchPeople(text, guard, [=, this](
				const QVector<Data::Profile> &people) {
			if (request != _request) {
				return;
			}
			auto rows = QVector<Boxes::PeerListRow>();
			for (const auto &profile : people) {
				if (!profile.deleted && profile.id != session->userId()) {
					rows.push_back(Boxes::PeerListRow::FromUser(profile));
				}
			}
			if (!rows.isEmpty() || !Api::Auth::IsValidHandle(handle)) {
				delegate()->setRows(rows);
				return;
			}
			// An exact @handle the search index does not know yet.
			session->data().graph().profileByHandle(
				handle,
				guard,
				[=, this](const Data::Profile &profile) {
					if (request != _request) {
						return;
					}
					auto found = QVector<Boxes::PeerListRow>();
					if (!profile.deleted && profile.id != session->userId()) {
						found.push_back(Boxes::PeerListRow::FromUser(profile));
					}
					delegate()->setRows(found);
				},
				[=, this](const Api::Error &) {
					if (request == _request) {
						delegate()->setRows({});
					}
				});
		});
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		if (_opening) {
			return;
		}
		if (row.id == kNewGroupRowId) {
			const auto controller = _controller;
			delegate()->closeBox();
			controller->openRoute(Window::Routes::NewGroup());
			return;
		}
		_opening = true;
		const auto controller = _controller;
		const auto session = this->session();
		const auto box = delegate()->peerListBox();
		const auto guard = base::Guard(box);
		const auto done = [=](const Data::Chat &chat) {
			const auto route = Window::Routes::Chat(chat.id);
			box->closeBox();
			controller->openRoute(route);
		};
		const auto fail = [=, this](const Api::Error &error) {
			_opening = false;
			Support::ShowError(controller, error);
		};
		if (row.id == kSavedRowId) {
			session->data().chats().openSaved(guard, done, fail);
		} else {
			session->data().chats().openDirect(row.id, guard, done, fail);
		}
	}

private:
	void add(const Data::User &user, const QString &fallbackName) {
		if (!user.valid() || _known.contains(user.id)) {
			return;
		}
		_known.insert(user.id);
		auto row = Boxes::PeerListRow::FromUser(user);
		if (row.title.isEmpty()) {
			row.title = fallbackName;
		}
		_base.push_back(row);
	}

	Window::SessionController *_controller = nullptr;
	QVector<Boxes::PeerListRow> _base;
	QSet<QString> _known;
	int _request = 0;
	bool _opening = false;

};

} // namespace

void ShowNewMessageBox(Window::SessionController *controller) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<NewMessageController>(controller)));
}

} // namespace Dialogs
