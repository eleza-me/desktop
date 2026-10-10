// The small boxes of «الی»: who liked / disliked a post, and the report form
// with the reasons the server offers.
#include "eli/eli_actions.h"

#include "boxes/peer_list_box.h"
#include "boxes/report_box.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "eli/eli_post_card.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

namespace Eli {
namespace {

class ReactorsController final : public Boxes::PeerListController {
public:
	ReactorsController(
		Window::SessionController *controller,
		const QString &postId,
		bool dislikes)
	: _controller(controller)
	, _postId(postId)
	, _dislikes(dislikes) {
	}

	QString title() const override {
		const auto name = _dislikes
			? tr::lng_profile_dislikes(tr::now)
			: tr::lng_profile_likes(tr::now);
		const auto post = _controller->session().data().feed().post(_postId);
		const auto count = !post
			? 0
			: _dislikes
			? (post->counts.hasDislikes ? post->counts.dislikes : 0)
			: post->counts.likes;
		return (count > 0)
			? tr::lng_eli_reactors_title(
				tr::now,
				lt_title,
				name,
				lt_n,
				Lang::CompactCount(count))
			: name;
	}

	void prepare() override {
		delegate()->setLoading(true);
		load();
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		const auto controller = _controller;
		const auto id = row.id;
		delegate()->closeBox();
		OpenProfile(controller, id);
	}

	QString emptyText() const override {
		if (_hidden) {
			return tr::lng_profile_list_not_shown(tr::now);
		} else if (!_error.isEmpty()) {
			return _error;
		} else if (delegate() && !delegate()->searchQuery().isEmpty()) {
			return tr::lng_profile_nobody_found(tr::now);
		}
		return _dislikes
			? tr::lng_profile_no_dislikes_yet(tr::now)
			: tr::lng_profile_no_likes_yet(tr::now);
	}

	void loadMore() override {
		if (!_cursor.isEmpty()) {
			load();
		}
	}

private:
	void load() {
		if (_loading || _done) {
			return;
		}
		_loading = true;
		const auto first = !_loadedOnce;
		_controller->session().data().feed().reactors(
			_postId,
			_dislikes ? Data::Feed::Reaction::Dislike : Data::Feed::Reaction::Like,
			_cursor,
			base::Guard(delegate()->peerListBox()),
			[=, this](const Data::GraphList &list) {
				_loading = false;
				_loadedOnce = true;
				_controller->session().data().users().seed(list.users);
				auto rows = QVector<Boxes::PeerListRow>();
				for (const auto &user : list.users) {
					if (user.deleted || _known.contains(user.id)) {
						continue;
					}
					_known.insert(user.id);
					auto row = Boxes::PeerListRow::FromUser(user);
					if (!user.handle.isEmpty()) {
						row.subtitle = IsolatedHandle(user.handle);
					}
					rows.push_back(row);
				}
				_cursor = list.nextCursor;
				_done = _cursor.isEmpty() || list.users.isEmpty();
				if (first) {
					delegate()->setRows(rows);
				} else {
					delegate()->appendRows(rows);
				}
				// A short page (blocked people are left out): go on.
				if (rows.isEmpty() && !_done) {
					load();
				}
			},
			[this](const Api::Error &error) {
				_loading = false;
				_done = true;
				_hidden = (error.code == u"post.reactions_hidden");
				_error = Support::ErrorText(error, tr::lng_error_load(tr::now));
				if (!_loadedOnce) {
					delegate()->setRows({});
				}
			});
	}

	Window::SessionController *_controller = nullptr;
	QString _postId;
	bool _dislikes = false;
	QString _cursor;
	QSet<QString> _known;
	bool _loading = false;
	bool _loadedOnce = false;
	bool _done = false;
	bool _hidden = false;
	QString _error;

};

} // namespace

void OpenReactors(
		Window::SessionController *controller,
		const QString &postId,
		bool dislikes) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<ReactorsController>(controller, postId, dislikes)));
}

void OpenReport(Window::SessionController *controller, ReportTarget target) {
	// The form is shared with the profile and the conversation topics.
	auto args = Boxes::ReportArgs();
	args.kind = target.kind;
	args.id = target.id;
	args.offerBlockUserId = target.offerBlockUserId;
	Boxes::ShowReportBox(controller, std::move(args));
}

} // namespace Eli
