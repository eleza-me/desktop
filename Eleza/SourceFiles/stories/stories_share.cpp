#include "stories/stories_share.h"

#include "api/api_client_config.h"
#include "boxes/peer_list_box.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>

namespace Stories {
namespace {

constexpr auto kRecentLimit = 50;

[[nodiscard]] QString ReasonText(Data::StoryShareReason reason) {
	switch (reason) {
	case Data::StoryShareReason::Privacy:
		return tr::lng_stories_share_why_privacy(tr::now);
	case Data::StoryShareReason::NotAllowed:
		return tr::lng_stories_share_why_not_allowed(tr::now);
	case Data::StoryShareReason::Limit:
		return tr::lng_stories_share_why_limit(tr::now);
	case Data::StoryShareReason::Unavailable:
		return tr::lng_stories_share_why_unavailable(tr::now);
	case Data::StoryShareReason::Failed:
		break;
	}
	return tr::lng_stories_share_why_failed(tr::now);
}

class ShareController final : public Boxes::PeerListController {
public:
	ShareController(Window::SessionController *window, QString storyId)
	: _window(window)
	, _storyId(std::move(storyId)) {
	}

	QString title() const override {
		return tr::lng_stories_share_to(tr::now);
	}
	QString searchPlaceholder() const override {
		return tr::lng_stories_share_search(tr::now);
	}
	QString emptyText() const override {
		return tr::lng_nothing_found(tr::now);
	}
	bool searchRemotely() const override {
		return true;
	}
	bool multiSelect() const override {
		return true;
	}
	int selectionLimit() const override {
		return _window->session().data().stories().shareCap();
	}
	void selectionLimitReached() override {
		_window->showToast(tr::lng_stories_share_cap(
			tr::now,
			lt_n,
			Lang::Num(selectionLimit())));
	}
	QString submitText() const override {
		return tr::lng_send(tr::now);
	}
	QString commentPlaceholder() const override {
		return tr::lng_stories_share_comment(tr::now);
	}
	int commentMaxLength() const override {
		return _window->session().account().clientConfig().maxMessageLength();
	}

	void prepare() override {
		const auto session = this->session();
		auto &users = session->data().users();
		const auto me = session->userId();

		// Saved Messages: my own id sends it there.
		auto saved = Boxes::PeerListRow();
		saved.id = me;
		saved.title = tr::lng_saved_messages(tr::now);
		saved.special = Ui::UserpicKind::Saved;
		add(saved);

		auto count = 0;
		for (const auto &chat : session->data().chats().list(
				Data::Chats::Section::Messages)) {
			if (chat.saved
				|| chat.anonymous
				|| chat.isBot
				|| chat.peerDeleted
				|| !chat.canSend) {
				continue;
			}
			if (chat.isGroup) {
				add(Boxes::PeerListRow::FromChat(session, chat));
			} else if (!chat.peerUserId.isEmpty() && chat.peerUserId != me) {
				auto user = users.get(chat.peerUserId);
				if (!user.valid()) {
					user.id = chat.peerUserId;
					user.displayName = chat.title;
				}
				add(Boxes::PeerListRow::FromUser(user));
			} else {
				continue;
			}
			if (++count >= kRecentLimit) {
				break;
			}
		}
		showBase();

		session->data().graph().following(
			me,
			QString(),
			base::Guard(delegate()->peerListBox()),
			[this](const Data::GraphList &list) {
				for (const auto &profile : list.users) {
					if (!profile.deleted && !profile.me) {
						add(Boxes::PeerListRow::FromUser(profile));
					}
				}
				if (delegate()->searchQuery().isEmpty()) {
					showBase();
				}
			},
			[](const Api::Error &) {
			});
	}

	void searchQueryChanged(const QString &query) override {
		const auto request = ++_request;
		auto text = query.trimmed();
		if (text.isEmpty()) {
			showBase();
			return;
		}
		// What is listed already first (groups only come from here)…
		const auto needle = (text.startsWith(QLatin1Char('@'))
			? text.mid(1)
			: text).toLower();
		auto rows = QVector<Boxes::PeerListRow>();
		auto shown = QSet<QString>();
		for (const auto &row : std::as_const(_base)) {
			if (row.title.toLower().contains(needle)
				|| row.subtitle.toLower().contains(needle)) {
				rows.push_back(row);
				shown.insert(row.id);
			}
		}
		setShown(rows);
		while (text.startsWith(QLatin1Char('@'))) {
			text.remove(0, 1);
		}
		if (text.isEmpty()) {
			return;
		}
		// …then the people the server finds.
		delegate()->setLoading(rows.isEmpty());
		const auto session = this->session();
		session->data().graph().searchPeople(
			text,
			base::Guard(delegate()->peerListBox()),
			[=, this](const QVector<Data::Profile> &people) {
				if (request != _request) {
					return;
				}
				auto found = rows;
				auto known = shown;
				for (const auto &profile : people) {
					if (profile.deleted
						|| profile.id == session->userId()
						|| known.contains(profile.id)) {
						continue;
					}
					known.insert(profile.id);
					const auto row = Boxes::PeerListRow::FromUser(profile);
					remember(row);
					found.push_back(row);
				}
				setShown(found);
			});
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		Q_UNUSED(row);
	}

	void submit(const QStringList &ids) override {
		if (ids.isEmpty()) {
			return;
		}
		const auto window = _window;
		const auto storyId = _storyId;
		const auto names = _titles;
		const auto box = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		const auto list = delegate(); // the box itself: alive with `box`
		list->setSubmitLoading(true);
		// The answer is shown even when the box was closed meanwhile.
		window->session().data().stories().share(
			storyId,
			ids,
			list->commentText(),
			window,
			[=](const Data::StoryShareResult &result) {
				if (box) {
					box->closeBox();
				}
				ShowResult(window, result, names);
			},
			[=](const Api::Error &error) {
				const auto whole = (error.code == u"story.share_not_allowed")
					|| error.notFound();
				if (box) {
					if (whole) {
						box->closeBox();
					} else {
						list->setSubmitLoading(false);
					}
				}
				if (error.code == u"story.share_not_allowed") {
					window->showToast(tr::lng_stories_share_not_allowed(tr::now));
				} else if (error.notFound()) {
					window->showToast(tr::lng_stories_unavailable(tr::now));
				} else {
					Support::ShowError(window, error);
				}
			});
	}

private:
	static void ShowResult(
			Window::SessionController *window,
			const Data::StoryShareResult &result,
			const QHash<QString, QString> &names) {
		if (result.notSent.isEmpty()) {
			window->showToast(tr::lng_stories_share_sent(tr::now));
			return;
		}
		auto &users = window->session().data().users();
		auto lines = QStringList();
		for (const auto &entry : result.notSent) {
			auto name = names.value(entry.to);
			if (name.isEmpty()) {
				const auto user = users.peek(entry.to);
				name = user.displayName.isEmpty()
					? (user.handle.isEmpty()
						? tr::lng_stories_share_someone(tr::now)
						: Lang::Handle(user.handle))
					: user.displayName;
			}
			lines.push_back(tr::lng_stories_share_not_sent_line(
				tr::now,
				lt_name,
				Lang::Isolated(name),
				lt_reason,
				ReasonText(entry.reason)));
		}
		const auto title = result.sent.isEmpty()
			? tr::lng_stories_share_none_sent(tr::now)
			: tr::lng_stories_share_sent_some(
				tr::now,
				lt_count,
				double(result.sent.size()));
		window->show(Ui::MakeInformBox(lines.join(QLatin1Char('\n')), title));
	}

	void remember(const Boxes::PeerListRow &row) {
		if (!row.title.isEmpty()) {
			_titles.insert(row.id, row.title);
		}
	}
	void add(const Boxes::PeerListRow &row) {
		if (row.id.isEmpty() || _known.contains(row.id)) {
			return;
		}
		_known.insert(row.id);
		remember(row);
		_base.push_back(row);
	}
	void showBase() {
		setShown(_base);
	}
	void setShown(const QVector<Boxes::PeerListRow> &rows) {
		// What is ticked stays ticked through every search.
		const auto selected = delegate()->selectedIds();
		delegate()->setRows(rows);
		for (const auto &id : selected) {
			delegate()->setSelected(id, true);
		}
	}

	Window::SessionController *_window = nullptr;
	QString _storyId;
	QVector<Boxes::PeerListRow> _base;
	QSet<QString> _known;
	QHash<QString, QString> _titles;
	int _request = 0;

};

} // namespace

void ShowShareBox(
		Window::SessionController *controller,
		const QString &storyId) {
	if (storyId.isEmpty()) {
		return;
	}
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<ShareController>(controller, storyId)));
}

} // namespace Stories
