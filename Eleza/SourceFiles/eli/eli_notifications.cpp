// Notifications (route eli/notifications): «همه / نام‌بردن‌ها / گفتگو /
// درخواست‌ها», grouped rows with the faces of the people behind them, follow
// back / accept / decline right in the row, the follow requests banner and its
// page. A row opens what it is about (a comment: its post at that comment),
// the person's photo and name open their profile, a grouped follow row opens
// «دنبال‌کنندگان تازه» with everyone it stands for; a follow back shows
// whether I follow them already (it waits for the relationship). A notification that arrives while the screen is open shows at the top
// at once (the unread count is pushed; the first page is read again and
// merged). Leaving the screen marks everything read, and so does the button.
#include "eli/eli_actions.h"

#include "base/timer.h"
#include "boxes/peer_list_box.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_notifications.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "eli/eli_logic.h"
#include "eli/eli_post_card.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/chips.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtGui/QMouseEvent>

namespace Eli {
namespace {

constexpr auto kPageSize = 30;
constexpr auto kMaxFaces = 6;

enum class NotifHit {
	None,
	Body,
	Face,
	Name, // the first actor's name in the line

	Primary, // follow back / accept
	Secondary, // decline
};

[[nodiscard]] const style::icon &KindIcon(const QString &kind) {
	if (kind == u"like") {
		return st::eliNotifIconLike;
	} else if (kind == u"reply") {
		return st::eliNotifIconReply;
	} else if (kind == u"mention") {
		return st::eliNotifIconMention;
	} else if (kind == u"repost") {
		return st::eliNotifIconRepost;
	} else if (kind == u"quote") {
		return st::eliNotifIconQuote;
	} else if (kind == u"follow") {
		return st::eliNotifIconFollow;
	} else if (kind == u"follow_request") {
		return st::eliNotifIconRequest;
	} else if (kind == u"follow_accepted") {
		return st::eliNotifIconFollowing;
	} else if (kind == u"post" || kind == u"new_post") {
		return st::eliNotifIconPost;
	} else if (kind == u"conversation_joined") {
		return st::eliNotifIconListener;
	}
	return st::eliNotifIconOther;
}

[[nodiscard]] bool KnownKind(const QString &kind) {
	return (&KindIcon(kind) != &st::eliNotifIconOther);
}

struct Who {
	QString text;
	QString name; // the first actor's, isolated as it is in `text`
	bool person = false; // the name opens a profile
};

[[nodiscard]] Who WhoText(Main::Session &session, const Data::NotificationEntry &entry) {
	const auto first = entry.actors.isEmpty() ? QString() : entry.actors.front();
	auto name = QString();
	auto person = false;
	if (first.isEmpty()) {
		name = tr::lng_eli_notif_a_listener(tr::now);
	} else {
		const auto user = session.data().users().get(first);
		name = user.deleted
			? tr::lng_deleted_account(tr::now)
			: user.displayName.isEmpty()
			? tr::lng_eli_notif_someone(tr::now)
			: user.displayName;
		person = !user.deleted;
	}
	// Only the name is isolated: it keeps its own direction, while «و N نفر
	// دیگر» / "and N others" reads with the sentence (isolating the whole
	// phrase put "and 1 other" before a Persian name in English).
	name = Lang::Isolated(name);
	const auto total = std::max(entry.actorCount, int(entry.actors.size()));
	const auto others = std::max(total - 1, 0);
	auto result = Who();
	result.text = (others > 0)
		? tr::lng_eli_notif_who_others(tr::now, lt_count, double(others), lt_name, name)
		: name;
	result.name = name;
	result.person = person;
	return result;
}

// ---- the follow back of a new follower

// A notification does not say whether I follow them already (I may have
// followed back elsewhere): the button waits for the relationship and never
// offers «تو هم دنبال کن» to somebody I follow.
enum class FollowBack {
	Unknown, // not asked yet: no button
	Offer, // «تو هم دنبال کن» (also when the server did not answer)
	Following, // «دنبال می‌کنی»: a click unfollows (after a confirmation)
	Requested, // «درخواست فرستاده شد»: a click withdraws the request
};

[[nodiscard]] FollowBack FollowBackOf(const QString &follow) {
	if (follow == u"following") {
		return FollowBack::Following;
	} else if (follow == u"requested") {
		return FollowBack::Requested;
	}
	return FollowBack::Offer;
}

[[nodiscard]] QString FollowBackText(FollowBack state) {
	switch (state) {
	case FollowBack::Following: return tr::lng_following(tr::now);
	case FollowBack::Requested: return tr::lng_follow_requested(tr::now);
	case FollowBack::Offer: return tr::lng_follow_back(tr::now);
	case FollowBack::Unknown: break;
	}
	return QString();
}

// Follows, unfollows or withdraws by the state shown; `done` gets the new
// state (`failed` the old one is to be shown again).
void ToggleFollowBack(
		Window::SessionController *controller,
		QObject *guard,
		const QString &userId,
		const QString &name,
		FollowBack shown,
		Fn<void()> started,
		Fn<void(const QString &follow)> done,
		Fn<void()> failed) {
	const auto weak = QPointer<QObject>(guard);
	const auto apply = [=] {
		if (!weak) {
			return;
		}
		started();
		auto &graph = controller->session().data().graph();
		const auto answered = [=](const Data::GraphState &state) {
			done(state.follow.isEmpty() ? u"none"_s : state.follow);
		};
		const auto fail = [=](const Api::Error &error) {
			failed();
			Support::ShowError(controller, error);
		};
		if (shown == FollowBack::Offer) {
			graph.follow(userId, base::Guard(weak.data()), answered, fail);
		} else {
			graph.unfollow(userId, base::Guard(weak.data()), answered, fail);
		}
	};
	if (shown != FollowBack::Following) {
		apply();
		return;
	}
	auto args = Ui::ConfirmBoxArgs();
	args.text = tr::lng_search_unfollow_sure(
		tr::now,
		lt_name,
		Lang::Isolated(name));
	args.confirmText = tr::lng_unfollow(tr::now);
	args.cancelText = tr::lng_never_mind(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		apply();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

// «دنبال‌کنندگان تازه»: everyone a grouped follow row stands for (the server
// lists the newest 20 of a row), each with a follow back; the rest is
// counted at the end. A click on somebody opens their profile.
class NewFollowersController final : public Boxes::PeerListController {
public:
	NewFollowersController(
		Window::SessionController *controller,
		const Data::NotificationEntry &entry)
	: _controller(controller)
	, _entry(entry) {
	}

	QString title() const override {
		return tr::lng_eli_new_followers(tr::now);
	}

	void prepare() override {
		const auto box = delegate()->peerListBox();
		auto &data = _controller->session().data();
		QObject::connect(
			&data.graph(),
			&Data::Graph::relationshipChanged,
			box,
			[this](const QString &userId, const Data::GraphState &state) {
				if (_states.contains(userId) && !_busy.contains(userId)) {
					_states[userId] = FollowBackOf(state.follow);
					refresh(userId);
				}
			});
		auto rows = QVector<Boxes::PeerListRow>();
		for (const auto &id : _entry.actors) {
			if (id.isEmpty() || _states.contains(id)) {
				continue;
			}
			_states.insert(id, FollowBack::Unknown);
			rows.push_back(rowFor(id));
			if (id == _controller->session().userId()) {
				continue;
			}
			data.graph().relationship(id, base::Guard(box), [=, this](
					const Data::GraphState &state) {
				_states[id] = FollowBackOf(state.follow);
				refresh(id);
			}, [=, this](const Api::Error &) {
				_states[id] = FollowBack::Offer;
				refresh(id);
			});
		}
		const auto rest = std::max(_entry.actorCount, int(_entry.actors.size()))
			- int(_entry.actors.size());
		if (rest > 0) {
			auto note = Boxes::PeerListRow();
			note.id = u"more"_s;
			note.note = true;
			note.title = tr::lng_eli_notif_and_more(tr::now, lt_count, double(rest));
			rows.push_back(note);
		}
		delegate()->setRows(rows);
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		const auto controller = _controller;
		const auto id = row.id;
		delegate()->closeBox();
		OpenProfile(controller, id);
	}

	void rowActionClicked(const Boxes::PeerListRow &row) override {
		const auto id = row.id;
		const auto shown = _states.value(id, FollowBack::Unknown);
		if (shown == FollowBack::Unknown || _busy.contains(id)) {
			return;
		}
		ToggleFollowBack(
			_controller,
			delegate()->peerListBox(),
			id,
			row.title,
			shown,
			[=, this] {
				_busy.insert(id);
				refresh(id);
			},
			[=, this](const QString &follow) {
				_busy.remove(id);
				_states[id] = FollowBackOf(follow);
				refresh(id);
			},
			[=, this] {
				_busy.remove(id);
				refresh(id);
			});
	}

private:
	[[nodiscard]] Boxes::PeerListRow rowFor(const QString &id) const {
		const auto user = _controller->session().data().users().get(id);
		auto row = Boxes::PeerListRow::FromUser(user);
		row.id = id;
		row.userId = user.deleted ? QString() : id;
		if (!user.handle.isEmpty() && !user.deleted) {
			row.subtitle = IsolatedHandle(user.handle);
		}
		const auto state = _states.value(id, FollowBack::Unknown);
		if (!user.deleted) {
			row.action = FollowBackText(state);
			row.actionFilled = (state == FollowBack::Offer);
			row.actionBusy = _busy.contains(id);
		}
		return row;
	}

	void refresh(const QString &id) {
		delegate()->updateRow(rowFor(id));
	}

	Window::SessionController *_controller = nullptr;
	Data::NotificationEntry _entry;
	QHash<QString, FollowBack> _states;
	QSet<QString> _busy;

};

void ShowNewFollowers(
		Window::SessionController *controller,
		const Data::NotificationEntry &entry) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<NewFollowersController>(controller, entry)));
}

[[nodiscard]] QString LineText(
		const Data::NotificationEntry &entry,
		const QString &who,
		bool accepted) {
	const auto &kind = entry.kind;
	if (accepted) {
		return tr::lng_eli_notif_now_follows(tr::now, lt_who, who);
	} else if (kind == u"like") {
		return (entry.actorCount > 1)
			? tr::lng_eli_notif_like_many(tr::now, lt_who, who)
			: tr::lng_eli_notif_like(tr::now, lt_who, who);
	} else if (kind == u"reply") {
		return Logic::AnswersComment(entry.refs)
			? tr::lng_eli_notif_reply_comment(tr::now, lt_who, who)
			: tr::lng_eli_notif_reply(tr::now, lt_who, who);
	} else if (kind == u"mention") {
		return tr::lng_eli_notif_mention(tr::now, lt_who, who);
	} else if (kind == u"repost") {
		return tr::lng_eli_notif_repost(tr::now, lt_who, who);
	} else if (kind == u"quote") {
		return tr::lng_eli_notif_quote(tr::now, lt_who, who);
	} else if (kind == u"follow") {
		return tr::lng_eli_notif_follow(tr::now, lt_who, who);
	} else if (kind == u"follow_request") {
		return tr::lng_eli_notif_follow_request(tr::now, lt_who, who);
	} else if (kind == u"follow_accepted") {
		return tr::lng_eli_notif_follow_accepted(tr::now, lt_who, who);
	} else if (kind == u"post" || kind == u"new_post") {
		return tr::lng_eli_notif_post(tr::now, lt_who, who);
	} else if (kind == u"conversation_joined") {
		return tr::lng_eli_notif_joined(tr::now, lt_who, who);
	}
	return QString();
}

// The route a row leads to; empty = the profile of its first actor.
[[nodiscard]] QString TargetRoute(const Data::NotificationEntry &entry) {
	const auto ref = [&](const char *name) {
		return entry.refs.value(QString::fromLatin1(name));
	};
	const auto post = [](const QString &id) {
		return id.isEmpty() ? QString() : Window::Routes::Post(id);
	};
	const auto &kind = entry.kind;
	if (kind == u"reply" || kind == u"mention") {
		// The post at that comment: its thread first, the comment lit up.
		const auto target = Logic::CommentTargetOf(kind, entry.refs);
		return target.post.isEmpty()
			? QString()
			: Window::Routes::Post(target.post, target.rootHint);
	} else if (kind == u"quote") {
		return post(ref("quote").isEmpty() ? ref("post") : ref("quote"));
	} else if (kind == u"conversation_joined") {
		return ref("chat").isEmpty() ? QString() : Window::Routes::Chat(ref("chat"));
	} else if (kind == u"follow" || kind == u"follow_request" || kind == u"follow_accepted") {
		return QString();
	}
	return post(ref("post"));
}

class NotifList final : public Ui::RpWidget {
public:
	NotifList(QWidget *parent, Window::SessionController *controller)
	: RpWidget(parent)
	, _controller(controller)
	, _userpics(
		&controller->session(),
		[this](const QString &) { update(); },
		this)
	, _minute([this] {
		for (auto &row : _rows) {
			rebuild(row);
		}
		relayout();
	}) {
		setMouseTracking(true);
		auto &data = controller->session().data();
		connect(&data.users(), &Data::Users::changed, this, [this](const QStringList &ids) {
			auto changed = false;
			for (auto &row : _rows) {
				for (const auto &actor : row.entry.actors) {
					if (ids.contains(actor)) {
						rebuild(row);
						changed = true;
						break;
					}
				}
			}
			if (changed) {
				relayout();
			}
		});
		connect(&data.graph(), &Data::Graph::relationshipChanged, this, [this](
				const QString &userId,
				const Data::GraphState &state) {
			if (_follow.contains(userId) && !_followBusy.contains(userId)) {
				setFollow(userId, state.follow);
			}
		});
		connect(&data.feed(), &Data::Feed::postChanged, this, [this](const QString &id) {
			auto changed = false;
			for (auto &row : _rows) {
				if (row.snippetPost == id) {
					rebuild(row);
					changed = true;
				}
			}
			if (changed) {
				relayout();
			}
		});
		_minute.callEach(60 * 1000);
	}

	void setEntries(const QVector<Data::NotificationEntry> &entries) {
		auto old = std::move(_rows);
		_rows.clear();
		for (const auto &entry : entries) {
			if (!KnownKind(entry.kind)) {
				continue;
			}
			auto row = Row();
			row.entry = entry;
			for (auto &was : old) {
				if (was.entry.id == entry.id) {
					row.accepted = was.accepted;
					row.declined = was.declined;
					row.busy = was.busy;
				}
			}
			rebuild(row);
			_rows.push_back(std::move(row));
		}
		_over = -1;
		_overHit = NotifHit::None;
		relayout();
	}

	void markAllRead() {
		for (auto &row : _rows) {
			row.entry.read = true;
		}
		update();
	}

	[[nodiscard]] int count() const {
		return int(_rows.size());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_column = std::min(newWidth, int(st::eliCardMaxWidth));
		_left = (newWidth - _column) / 2;
		auto top = 0;
		for (auto &row : _rows) {
			row.top = top;
			layout(row);
			top += row.height;
		}
		return top;
	}

	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		_visibleTop = visibleTop;
		_visibleBottom = visibleBottom;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto clip = e->rect();
		for (auto i = 0; i != int(_rows.size()); ++i) {
			const auto &row = _rows[i];
			if (row.top + row.height <= clip.top()) {
				continue;
			} else if (row.top >= clip.top() + clip.height()) {
				break;
			}
			paintRow(p, row, i);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		updateHover(e->pos());
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		setHover(-1, NotifHit::None, -1);
	}

	void mousePressEvent(QMouseEvent *e) override {
		updateHover(e->pos());
		if (e->button() == Qt::LeftButton) {
			_pressed = _over;
			_pressedHit = _overHit;
			_pressedFace = _overFace;
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		updateHover(e->pos());
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto index = std::exchange(_pressed, -1);
		const auto hit = std::exchange(_pressedHit, NotifHit::None);
		const auto face = std::exchange(_pressedFace, -1);
		if (index < 0 || index != _over || hit != _overHit || face != _overFace) {
			return;
		}
		activate(index, hit, face);
	}

private:
	struct Row {
		Data::NotificationEntry entry;
		Ui::Text::String text;
		Ui::Text::String snippet;
		QString snippetPost;
		QString primary;
		QString secondary;
		bool primaryOutline = false;
		int nameAt = -1; // the first actor's name in `text`
		int nameLength = 0;
		bool accepted = false;
		bool declined = false;
		bool busy = false;
		int top = 0;
		int height = 0;
		// Logical (leading side) rects inside the column, relative to the row.
		std::vector<QRect> faces;
		QRect textRect;
		QRect snippetRect;
		QRect primaryRect;
		QRect secondaryRect;
		int moreFaces = 0;
	};

	void rebuild(Row &row) {
		auto &session = _controller->session();
		// The sentence reads in the interface's direction whatever script the
		// name is in; the name keeps its own.
		const auto who = WhoText(session, row.entry);
		const auto line = LineText(row.entry, who.text, row.accepted);
		const auto time = tr::lng_eli_meta_separator(tr::now) + Lang::RelativeTime(
			QDateTime::fromMSecsSinceEpoch(row.entry.updatedAt));
		auto marked = Ui::Text::TextWithEntities();
		marked.text = QChar(style::RightToLeft() ? 0x200F : 0x200E) + line + time;
		const auto at = marked.text.indexOf(who.text);
		if (at >= 0) {
			marked.entities.push_back({ Ui::Text::EntityType::Bold, int(at), int(who.text.size()), QString() });
		}
		// The name opens their profile, as their picture does.
		row.nameAt = (at >= 0 && who.person)
			? int(marked.text.indexOf(who.name, at))
			: -1;
		row.nameLength = (row.nameAt >= 0) ? int(who.name.size()) : 0;
		row.text.setMarkedText(st::eliNotifTextStyle, marked);
		row.snippetPost = QString();
		row.snippet.clear();
		const auto &kind = row.entry.kind;
		auto snippetId = QString();
		if (kind == u"reply") {
			snippetId = row.entry.refs.value(u"reply"_s);
		} else if (kind == u"quote") {
			snippetId = row.entry.refs.value(u"quote"_s);
		}
		if (snippetId.isEmpty() && kind != u"conversation_joined") {
			snippetId = row.entry.refs.value(u"post"_s);
		}
		if (!snippetId.isEmpty()) {
			row.snippetPost = snippetId;
			const auto post = session.data().feed().post(snippetId);
			if (post && !post->deleted && !post->text.isEmpty()) {
				row.snippet.setMarkedText(st::eliNotifSnippetStyle, PostText(post->text, false));
			}
		}
		row.primary = QString();
		row.secondary = QString();
		row.primaryOutline = false;
		const auto hasActor = !row.entry.actors.isEmpty();
		if (kind == u"follow" && hasActor) {
			const auto actor = row.entry.actors.front();
			const auto state = followBack(actor);
			if (!session.data().users().get(actor).deleted
				&& actor != session.userId()) {
				row.primary = FollowBackText(state);
				row.primaryOutline = (state != FollowBack::Offer);
			}
		} else if (kind == u"follow_request" && hasActor && !row.accepted && !row.declined) {
			row.primary = tr::lng_eli_request_accept(tr::now);
			row.secondary = tr::lng_eli_request_decline(tr::now);
		}
	}

	void layout(Row &row) const {
		const auto &padding = st::eliNotifPadding;
		const auto contentLeft = padding.left()
			+ st::eliNotifGlyphWidth
			+ st::eliNotifGlyphSkip;
		const auto contentWidth = _column - contentLeft - padding.right() - st::eliNotifDot - 8;
		auto y = padding.top();
		row.faces.clear();
		const auto face = st::eliNotifFace;
		const auto fits = std::max((contentWidth + st::eliNotifFaceSkip) / (face + st::eliNotifFaceSkip), 1);
		const auto actors = int(row.entry.actors.size());
		const auto shown = std::min({ actors, int(kMaxFaces), fits });
		for (auto i = 0; i != std::max(shown, 1); ++i) {
			row.faces.push_back(QRect(
				contentLeft + i * (face + st::eliNotifFaceSkip),
				y,
				face,
				face));
		}
		row.moreFaces = std::max(
			std::max(row.entry.actorCount, actors) - std::max(shown, 1),
			0);
		y += face + st::eliNotifTextSkip;
		const auto textHeight = row.text.countHeight(contentWidth, 3);
		row.textRect = QRect(contentLeft, y, contentWidth, textHeight);
		y += textHeight;
		row.snippetRect = QRect();
		if (!row.snippet.isEmpty()) {
			y += 2;
			const auto height = row.snippet.countHeight(contentWidth, st::eliNotifSnippetLines);
			row.snippetRect = QRect(contentLeft, y, contentWidth, height);
			y += height;
		}
		row.primaryRect = QRect();
		row.secondaryRect = QRect();
		if (!row.primary.isEmpty()) {
			y += st::eliNotifButtonsTop;
			const auto width = st::eliNotifButtonFont->width(row.primary)
				+ 2 * st::eliNotifButtonPadding;
			row.primaryRect = QRect(contentLeft, y, width, st::eliNotifButtonHeight);
			if (!row.secondary.isEmpty()) {
				const auto second = st::eliNotifButtonFont->width(row.secondary)
					+ 2 * st::eliNotifButtonPadding;
				row.secondaryRect = QRect(
					contentLeft + width + st::eliNotifButtonSkip,
					y,
					second,
					st::eliNotifButtonHeight);
			}
			y += st::eliNotifButtonHeight;
		}
		row.height = y + padding.bottom() + 1;
	}

	[[nodiscard]] QRect physical(const Row &row, const QRect &logical) const {
		return style::rtlrect(logical, _column).translated(_left, row.top);
	}

	void paintRow(Painter &p, const Row &row, int index) {
		const auto &padding = st::eliNotifPadding;
		const auto whole = QRect(_left, row.top, _column, row.height);
		if (!row.entry.read) {
			p.fillRect(whole, st::eliNotifUnreadBg);
		}
		if (_over == index && _overHit != NotifHit::None) {
			p.fillRect(whole, st::eliNotifOverBg);
		}
		const auto &icon = KindIcon(row.entry.kind);
		icon.paint(
			p,
			_left + padding.left() + (st::eliNotifGlyphWidth - icon.width()) / 2,
			row.top + padding.top() + (st::eliNotifFace - icon.height()) / 2,
			width());
		for (auto i = 0; i != int(row.faces.size()); ++i) {
			const auto rect = physical(row, row.faces[i]);
			if (i < int(row.entry.actors.size())) {
				_userpics.paint(p, rect, row.entry.actors[i]);
			} else {
				_userpics.paintSpecial(p, rect, Ui::UserpicKind::Anonymous);
			}
		}
		if (row.moreFaces > 0 && !row.faces.empty()) {
			const auto last = row.faces.back();
			const auto text = tr::lng_eli_more_media(
				tr::now,
				lt_n,
				Lang::Num(row.moreFaces));
			p.setPen(st::eliNotifTimeFg);
			p.setFont(st::eliNotifMoreFacesFont);
			p.drawTextLeft(
				_left + last.x() + last.width() + 8,
				row.top + last.y() + (last.height() - st::eliNotifMoreFacesFont->height) / 2,
				width(),
				text);
		}
		{
			const auto rect = physical(row, row.textRect);
			p.setPen(st::eliNotifTextFg);
			auto context = Ui::Text::PaintContext();
			context.position = rect.topLeft();
			context.availableWidth = rect.width();
			context.elisionLines = 3;
			row.text.draw(p, context);
		}
		if (!row.snippetRect.isEmpty()) {
			const auto rect = physical(row, row.snippetRect);
			p.setPen(st::eliNotifSnippetFg);
			auto context = Ui::Text::PaintContext();
			context.position = rect.topLeft();
			context.availableWidth = rect.width();
			context.elisionLines = st::eliNotifSnippetLines;
			context.autoDirection = true;
			row.snippet.draw(p, context);
		}
		PainterHighQualityEnabler hq(p);
		if (!row.primaryRect.isEmpty()) {
			const auto rect = physical(row, row.primaryRect);
			const auto over = (_over == index && _overHit == NotifHit::Primary);
			p.setOpacity(row.busy ? 0.5 : 1.);
			if (row.primaryOutline) {
				p.setPen(st::borderStrong);
				p.setBrush(over ? st::stateHover->b : QBrush(Qt::NoBrush));
				p.drawRoundedRect(
					QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
					rect.height() / 2,
					rect.height() / 2);
				p.setPen(st::textPrimary);
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(over ? st::brandPressed : st::brandPrimary);
				p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
				p.setPen(st::textOnBrand);
			}
			p.setFont(st::eliNotifButtonFont);
			p.drawTextCentered(rect, row.primary);
			if (!row.secondaryRect.isEmpty()) {
				const auto second = physical(row, row.secondaryRect);
				const auto overSecond = (_over == index && _overHit == NotifHit::Secondary);
				p.setPen(st::borderStrong);
				p.setBrush(overSecond ? st::stateHover->b : QBrush(Qt::NoBrush));
				p.drawRoundedRect(
					QRectF(second).adjusted(0.5, 0.5, -0.5, -0.5),
					second.height() / 2,
					second.height() / 2);
				p.setPen(st::textPrimary);
				p.drawTextCentered(second, row.secondary);
			}
			p.setOpacity(1.);
		}
		if (!row.entry.read) {
			const auto dot = st::eliNotifDot;
			p.setPen(Qt::NoPen);
			p.setBrush(st::brandPrimary);
			p.drawEllipse(style::rtlrect(
				_left + _column - padding.right() - dot,
				row.top + padding.top() + (st::eliNotifFace - dot) / 2,
				dot,
				dot,
				width()));
		}
		p.fillRect(QRect(_left, row.top + row.height - 1, _column, 1), st::eliCardLineFg);
	}

	void setHover(int index, NotifHit hit, int face) {
		if (_over == index && _overHit == hit && _overFace == face) {
			return;
		}
		_over = index;
		_overHit = hit;
		_overFace = face;
		setCursor((hit != NotifHit::None) ? style::cur_pointer : style::cur_default);
		update();
	}

	void updateHover(QPoint position) {
		for (auto i = 0; i != int(_rows.size()); ++i) {
			const auto &row = _rows[i];
			if (position.y() < row.top || position.y() >= row.top + row.height) {
				continue;
			}
			if (position.x() < _left || position.x() >= _left + _column) {
				break;
			}
			if (physical(row, row.primaryRect).contains(position)) {
				setHover(i, NotifHit::Primary, -1);
				return;
			} else if (physical(row, row.secondaryRect).contains(position)) {
				setHover(i, NotifHit::Secondary, -1);
				return;
			}
			for (auto face = 0; face != int(row.faces.size()); ++face) {
				if (face < int(row.entry.actors.size())
					&& physical(row, row.faces[face]).contains(position)) {
					setHover(i, NotifHit::Face, face);
					return;
				}
			}
			if (overName(row, position)) {
				setHover(i, NotifHit::Name, 0);
				return;
			}
			setHover(i, NotifHit::Body, -1);
			return;
		}
		setHover(-1, NotifHit::None, -1);
	}

	[[nodiscard]] bool overName(const Row &row, QPoint position) const {
		if (row.nameAt < 0) {
			return false;
		}
		const auto rect = physical(row, row.textRect);
		if (!rect.contains(position)) {
			return false;
		}
		auto context = Ui::Text::PaintContext();
		context.position = rect.topLeft();
		context.availableWidth = rect.width();
		context.elisionLines = 3;
		const auto state = row.text.getState(position, context);
		return state.uponSymbol
			&& state.symbol >= row.nameAt
			&& state.symbol < row.nameAt + row.nameLength;
	}

	// What the follow back of `userId` shows; asks the server once.
	[[nodiscard]] FollowBack followBack(const QString &userId) {
		const auto i = _follow.constFind(userId);
		if (i != _follow.cend()) {
			return *i;
		}
		_follow.insert(userId, FollowBack::Unknown);
		_controller->session().data().graph().relationship(
			userId,
			base::Guard(this),
			[=, this](const Data::GraphState &state) {
				setFollow(userId, state.follow);
			},
			[=, this](const Api::Error &) {
				// Unanswered: the follow back is offered, as before.
				setFollow(userId, QString());
			});
		return FollowBack::Unknown;
	}

	void setFollow(const QString &userId, const QString &follow) {
		_follow[userId] = FollowBackOf(follow);
		auto changed = false;
		for (auto &row : _rows) {
			if (row.entry.kind == u"follow"
				&& !row.entry.actors.isEmpty()
				&& row.entry.actors.front() == userId) {
				rebuild(row);
				changed = true;
			}
		}
		if (changed) {
			relayout();
		}
	}

	void setFollowBusy(const QString &userId, bool busy) {
		if (busy) {
			_followBusy.insert(userId);
		} else {
			_followBusy.remove(userId);
		}
		for (auto &row : _rows) {
			if (row.entry.kind == u"follow"
				&& !row.entry.actors.isEmpty()
				&& row.entry.actors.front() == userId) {
				row.busy = busy;
			}
		}
		update();
	}

	[[nodiscard]] Row *find(const QString &id) {
		for (auto &row : _rows) {
			if (row.entry.id == id) {
				return &row;
			}
		}
		return nullptr;
	}

	void changed(const QString &id, Fn<void(Row &)> change) {
		if (const auto row = find(id)) {
			change(*row);
			rebuild(*row);
			relayout();
		}
	}

	void activate(int index, NotifHit hit, int face) {
		auto &row = _rows[index];
		const auto id = row.entry.id;
		const auto actor = row.entry.actors.isEmpty()
			? QString()
			: row.entry.actors.front();
		auto &graph = _controller->session().data().graph();
		const auto controller = _controller;
		const auto fail = [=, this](const Api::Error &error) {
			changed(id, [](Row &row) { row.busy = false; });
			Support::ShowError(controller, error);
		};
		switch (hit) {
		case NotifHit::None:
			break;
		case NotifHit::Face:
			if (face >= 0 && face < int(row.entry.actors.size())) {
				OpenProfile(_controller, row.entry.actors[face]);
			}
			break;
		case NotifHit::Name:
			if (!actor.isEmpty()) {
				OpenProfile(_controller, actor);
			}
			break;
		case NotifHit::Body: {
			row.entry.read = true;
			update();
			const auto route = TargetRoute(row.entry);
			if (!route.isEmpty()) {
				_controller->openRoute(route);
			} else if (row.entry.kind == u"follow" && row.entry.actors.size() > 1) {
				// «سمانه و ۳ نفر دیگر دنبالت کردند»: who they all are.
				ShowNewFollowers(_controller, row.entry);
			} else if (!actor.isEmpty()) {
				OpenProfile(_controller, actor);
			}
		} break;
		case NotifHit::Primary:
			if (row.busy || actor.isEmpty()) {
				break;
			}
			if (row.entry.kind == u"follow") {
				const auto shown = _follow.value(actor, FollowBack::Unknown);
				if (shown == FollowBack::Unknown) {
					break;
				}
				const auto name = _controller->session().data().users().get(actor).displayName;
				ToggleFollowBack(
					_controller,
					this,
					actor,
					name,
					shown,
					[=, this] { setFollowBusy(actor, true); },
					[=, this](const QString &follow) {
						setFollowBusy(actor, false);
						setFollow(actor, follow);
					},
					[=, this] { setFollowBusy(actor, false); });
				break;
			}
			row.busy = true;
			update();
			graph.acceptFollowRequest(actor, base::Guard(this), [=, this](const Data::GraphState &) {
				changed(id, [](Row &row) {
					row.busy = false;
					row.accepted = true;
				});
			}, fail);
			break;
		case NotifHit::Secondary:
			if (row.busy || actor.isEmpty()) {
				break;
			}
			row.busy = true;
			update();
			graph.declineFollowRequest(actor, base::Guard(this), [=, this](const Data::GraphState &) {
				changed(id, [](Row &row) {
					row.busy = false;
					row.declined = true;
				});
			}, fail);
			break;
		}
	}

	void relayout() {
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

	Window::SessionController *_controller = nullptr;
	Support::UserpicPainter _userpics;
	std::vector<Row> _rows;
	int _column = 0;
	int _left = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	int _over = -1;
	NotifHit _overHit = NotifHit::None;
	int _overFace = -1;
	int _pressed = -1;
	NotifHit _pressedHit = NotifHit::None;
	int _pressedFace = -1;
	QHash<QString, FollowBack> _follow;
	QSet<QString> _followBusy;
	base::Timer _minute;

};

// «درخواست‌های دنبال کردن» with the faces of those who wait.
class RequestsBanner final : public Ui::AbstractButton {
public:
	RequestsBanner(QWidget *parent, Main::Session *session)
	: AbstractButton(parent)
	, _userpics(session, [this](const QString &) { update(); }, this) {
	}

	void setUsers(const QVector<Data::Profile> &users) {
		_users = users;
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return _users.isEmpty() ? 0 : st::eliNotifBannerHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_users.isEmpty()) {
			return;
		}
		auto p = Painter(this);
		const auto column = std::min(width(), int(st::eliCardMaxWidth));
		const auto left = (width() - column) / 2;
		if (isOver()) {
			p.fillRect(QRect(left, 0, column, height()), st::eliNotifOverBg);
		}
		const auto &padding = st::eliNotifPadding;
		const auto face = st::eliNotifFace;
		const auto overlap = 10;
		const auto shown = std::min(int(_users.size()), 3);
		for (auto i = shown - 1; i >= 0; --i) {
			const auto rect = style::rtlrect(
				left + padding.left() + i * (face - overlap),
				(height() - face) / 2,
				face,
				face,
				width());
			{
				PainterHighQualityEnabler hq(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::bgCanvas);
				p.drawEllipse(rect.adjusted(-2, -2, 2, 2));
			}
			_userpics.paint(p, rect, _users[i].card());
		}
		const auto textLeft = left
			+ padding.left()
			+ face
			+ (shown - 1) * (face - overlap)
			+ 12;
		const auto top = (height() - st::eliNotifBannerTitleFont->height - st::eliNotifBannerSubFont->height - 2) / 2;
		p.setPen(st::textPrimary);
		p.setFont(st::eliNotifBannerTitleFont);
		p.drawTextLeft(textLeft, top, width(), tr::lng_eli_follow_requests(tr::now));
		p.setPen(st::textSecondary);
		p.setFont(st::eliNotifBannerSubFont);
		p.drawTextLeft(
			textLeft,
			top + st::eliNotifBannerTitleFont->height + 2,
			width(),
			tr::lng_eli_notif_requests_waiting(
				tr::now,
				lt_count,
				double(_users.size())));
		st::eliNotifBannerChevron.paint(
			p,
			left + column - padding.right() - st::eliNotifBannerChevron.width(),
			(height() - st::eliNotifBannerChevron.height()) / 2,
			width());
		p.fillRect(QRect(left, height() - 1, column, 1), st::eliCardLineFg);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		Q_UNUSED(was);
		Q_UNUSED(source);
		update();
	}

private:
	Support::UserpicPainter _userpics;
	QVector<Data::Profile> _users;

};

class NotificationsSection final : public Window::SectionWidget {
public:
	NotificationsSection(QWidget *parent, Window::SessionController *controller)
	: SectionWidget(parent, controller)
	, _bar(new Ui::TopBar(this))
	, _chips(new Ui::ChipsRow(this, st::eliNotifChips))
	, _scroll(new Ui::ScrollArea(this)) {
		_bar->setTitle(tr::lng_settings_notifications(tr::now));
		setTopBar(_bar);
		const auto settings = _bar->addAction(st::topBarSettingsButton);
		settings->setClickedCallback([=] {
			controller->openRoute(Window::Routes::Settings(u"notifications"_s));
		});
		Ui::InstallTooltip(settings, [] { return tr::lng_eli_notif_settings(tr::now); });
		const auto readAll = _bar->addAction(st::eliNotifReadAll);
		readAll->setClickedCallback([this] { markAllRead(true); });
		Ui::InstallTooltip(readAll, [] { return tr::lng_eli_notif_mark_read(tr::now); });

		refreshChips();
		connect(_chips, &Ui::ChipsRow::activated, this, [this](const QString &id) {
			if (id != _tab && !id.isEmpty()) {
				_tab = id;
				load(true);
			}
		});

		_content = _scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(_scroll));
		_banner = _content->add(
			Ui::object_ptr<RequestsBanner>(_content, &controller->session()));
		_banner->setClickedCallback([=] { OpenFollowRequests(controller); });
		_list = _content->add(Ui::object_ptr<NotifList>(_content, controller));
		_tail = _content->add(Ui::object_ptr<Ui::RpWidget>(_content));
		_scroll->setNearEdgeThreshold(400);
		connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
			if (!_entries.isEmpty() && !_failed) {
				load(false);
			}
		});

		auto &data = controller->session().data();
		connect(&data.notifications(), &Data::Notifications::unreadChanged, this, [this] {
			refreshChips();
			// Something new while the screen is open: it shows at the top.
			if (this->controller()->session().data().notifications().unread() > 0) {
				refreshTop();
			}
		});
		data.graph().followRequests(base::Guard(this), [this](const QVector<Data::Profile> &users) {
			_requests = int(users.size());
			_banner->setUsers(users);
			refreshChips();
		});
		load(true);
	}

	~NotificationsSection() {
		// Leaving marks everything read (as the web client does).
		controller()->session().data().notifications().markAllRead();
	}

	QString sectionKey() const override {
		return Window::Routes::EliNotifications();
	}

	void setInnerFocus() override {
		_scroll->setFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		_chips->resizeToWidth(width());
		_chips->move(0, _bar->height());
		const auto top = _bar->height() + _chips->height();
		_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
		refreshTail();
	}

private:
	void refreshChips() {
		const auto active = _tab;
		auto requests = _requests;
		if (!requests) {
			requests = controller()->session().data().notifications().kind(u"follow_request"_s);
		}
		if (_chips->count() && requests == _chipsRequests) {
			return;
		}
		_chipsRequests = requests;
		_chips->clearChips();
		_chips->addChip(u"all"_s, tr::lng_all(tr::now));
		_chips->addChip(u"mentions"_s, tr::lng_eli_mentions(tr::now));
		_chips->addChip(
			u"requests"_s,
			(requests > 0)
				? tr::lng_eli_notif_requests_count(tr::now, lt_n, Lang::Num(requests))
				: tr::lng_dialogs_requests(tr::now));
		_chips->setActive(active, anim::type::instant);
		_chips->resizeToWidth(width());
	}

	void load(bool reset) {
		if (reset) {
			++_generation;
			_loading = false;
			_before = 0;
			_done = false;
			_failed = false;
			_entries.clear();
			_list->setEntries({});
			_scroll->scrollToY(0);
		}
		if (_loading || _done) {
			return;
		}
		_loading = true;
		_failed = false;
		refreshTail();
		const auto generation = _generation;
		controller()->session().data().notifications().load(
			_tab,
			_before,
			base::Guard(this),
			[=, this](const QVector<Data::NotificationEntry> &page) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				const auto last = page.isEmpty() ? qint64(0) : page.back().updatedAt;
				auto added = 0;
				for (const auto &entry : page) {
					const auto known = std::any_of(
						_entries.cbegin(),
						_entries.cend(),
						[&](const Data::NotificationEntry &other) { return other.id == entry.id; });
					if (!known) {
						_entries.push_back(entry);
						++added;
					}
				}
				_done = page.isEmpty()
					|| !last
					|| !added
					|| (page.size() < kPageSize);
				// The server keeps microseconds and lists `updated_at < before`
				// in milliseconds: the rows of the last row's millisecond that
				// did not fit come with the next page (known ones are skipped).
				_before = last + 1;
				_list->setEntries(_entries);
				refreshTail();
			},
			[=, this](const Api::Error &error) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				_failed = true;
				_errorText = Support::ErrorText(error, tr::lng_eli_notif_failed(tr::now));
				refreshTail();
			});
	}

	void refreshTop() {
		if (_loading) {
			return;
		}
		const auto generation = _generation;
		controller()->session().data().notifications().load(
			_tab,
			0,
			base::Guard(this),
			[=, this](const QVector<Data::NotificationEntry> &page) {
				if (generation != _generation) {
					return;
				}
				auto merged = page;
				for (const auto &entry : _entries) {
					const auto fresh = std::any_of(
						merged.cbegin(),
						merged.cend(),
						[&](const Data::NotificationEntry &other) { return other.id == entry.id; });
					if (!fresh) {
						merged.push_back(entry);
					}
				}
				_entries = merged;
				_list->setEntries(_entries);
				refreshTail();
			});
	}

	void markAllRead(bool toast) {
		controller()->session().data().notifications().markAllRead();
		for (auto &entry : _entries) {
			entry.read = true;
		}
		_list->markAllRead();
		if (toast) {
			controller()->showToast(tr::lng_eli_notif_marked(tr::now));
		}
	}

	void refreshTail() {
		delete _placeholder;
		_placeholder = nullptr;
		delete _skeleton;
		_skeleton = nullptr;
		const auto area = std::max(_scroll->height() - _banner->height(), 160);
		auto height = int(st::eliListBottomSkip);
		if (_entries.isEmpty() && _loading) {
			_skeleton = new Ui::SkeletonList(_tail, Ui::SkeletonList::Kind::Chats);
			height = area;
		} else if (_failed) {
			auto descriptor = Ui::EmptyPlaceholderDescriptor();
			descriptor.icon = &st::emptyIconError;
			descriptor.title = tr::lng_eli_notif_failed(tr::now);
			descriptor.text = _errorText;
			descriptor.buttonText = tr::lng_try_again(tr::now);
			descriptor.buttonCallback = [this] {
				Ui::PostponeCall(this, [this] { load(_entries.isEmpty()); });
			};
			_placeholder = new Ui::EmptyPlaceholder(_tail, std::move(descriptor));
			height = _entries.isEmpty() ? area : 220;
		} else if (_entries.isEmpty() && !_loading) {
			auto descriptor = Ui::EmptyPlaceholderDescriptor();
			descriptor.icon = &st::emptyIconNotifications;
			descriptor.title = tr::lng_eli_nothing_new(tr::now);
			descriptor.text = tr::lng_eli_when_someone_replies_joins_conversation(tr::now);
			_placeholder = new Ui::EmptyPlaceholder(_tail, std::move(descriptor));
			height = area;
		}
		const auto column = std::min(_scroll->width(), int(st::eliCardMaxWidth));
		const auto left = (_scroll->width() - column) / 2;
		_tail->resize(_scroll->width(), height);
		if (_placeholder) {
			_placeholder->setGeometry(left, 0, column, height);
			_placeholder->show();
		}
		if (_skeleton) {
			_skeleton->setGeometry(left, 0, column, height);
			_skeleton->show();
		}
	}

	Ui::TopBar *_bar = nullptr;
	Ui::ChipsRow *_chips = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	RequestsBanner *_banner = nullptr;
	NotifList *_list = nullptr;
	Ui::RpWidget *_tail = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;

	QString _tab = u"all"_s;
	QVector<Data::NotificationEntry> _entries;
	qint64 _before = 0;
	bool _loading = false;
	bool _done = false;
	bool _failed = false;
	QString _errorText;
	int _generation = 0;
	int _requests = 0;
	int _chipsRequests = -1;

};

// ---- follow requests

class RequestRow final : public Ui::RpWidget {
public:
	RequestRow(
		QWidget *parent,
		Window::SessionController *controller,
		const Data::Profile &user)
	: RpWidget(parent)
	, _controller(controller)
	, _user(user)
	, _userpic(new Support::UserpicButton(
		this,
		&controller->session(),
		user.id,
		st::userpicSizeMedium))
	, _accept(new Ui::RoundButton(
		this,
		tr::lng_eli_request_accept(tr::now),
		st::smallRoundButton))
	, _decline(new Ui::RoundButton(
		this,
		tr::lng_eli_request_decline(tr::now),
		st::smallOutlineButton)) {
		controller->session().data().users().seed(QVector<Data::Profile>{ user });
		const auto id = user.id;
		const auto name = user.displayName;
		_userpic->setClickedCallback([=] { OpenProfile(controller, id); });
		auto &graph = controller->session().data().graph();
		const auto fail = [=, this](const Api::Error &error) {
			_accept->setLoading(false);
			_decline->setDisabled(false);
			Support::ShowError(controller, error);
		};
		_accept->setClickedCallback([=, this, &graph] {
			_accept->setLoading(true);
			_decline->setDisabled(true);
			graph.acceptFollowRequest(id, base::Guard(this), [=, this](const Data::GraphState &) {
				controller->showToast(tr::lng_eli_now_follows_you(tr::now, lt_name, name));
				if (removed) {
					removed();
				}
			}, fail);
		});
		_decline->setClickedCallback([=, this, &graph] {
			_decline->setDisabled(true);
			graph.declineFollowRequest(id, base::Guard(this), [this](const Data::GraphState &) {
				if (removed) {
					removed();
				}
			}, fail);
		});
	}

	Fn<void()> removed;

protected:
	int resizeGetHeight(int newWidth) override {
		const auto column = std::min(newWidth, int(st::eliCardMaxWidth));
		_left = (newWidth - column) / 2;
		_column = column;
		const auto &padding = st::eliNotifPadding;
		const auto height = st::eliRequestRowHeight;
		_userpic->moveToLeft(
			_left + padding.left(),
			(height - _userpic->height()) / 2,
			newWidth);
		_decline->resizeToNaturalWidth(column);
		_accept->resizeToNaturalWidth(column);
		_decline->moveToRight(
			_left + padding.right(),
			(height - _decline->height()) / 2,
			newWidth);
		_accept->moveToRight(
			_left + padding.right() + _decline->width() + 8,
			(height - _accept->height()) / 2,
			newWidth);
		return height;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::eliNotifPadding;
		const auto textLeft = _left + padding.left() + _userpic->width() + 12;
		const auto available = _column
			- (textLeft - _left)
			- padding.right()
			- _accept->width()
			- _decline->width()
			- 20;
		const auto hasHandle = !_user.handle.isEmpty();
		const auto top = hasHandle
			? (height() - st::eliRequestNameFont->height - st::eliRequestHandleFont->height - 2) / 2
			: (height() - st::eliRequestNameFont->height) / 2;
		p.setPen(st::textPrimary);
		p.setFont(st::eliRequestNameFont);
		p.drawTextLeft(
			textLeft,
			top,
			width(),
			st::eliRequestNameFont->elided(_user.displayName, std::max(available, 0)));
		if (hasHandle) {
			p.setPen(st::textTertiary);
			p.setFont(st::eliRequestHandleFont);
			p.drawTextLeft(
				textLeft,
				top + st::eliRequestNameFont->height + 2,
				width(),
				st::eliRequestHandleFont->elided(
					IsolatedHandle(_user.handle),
					std::max(available, 0)));
		}
		p.fillRect(QRect(_left, height() - 1, _column, 1), st::eliCardLineFg);
	}

private:
	Window::SessionController *_controller = nullptr;
	Data::Profile _user;
	Support::UserpicButton *_userpic = nullptr;
	Ui::RoundButton *_accept = nullptr;
	Ui::RoundButton *_decline = nullptr;
	int _left = 0;
	int _column = 0;

};

class FollowRequestsSection final : public Window::SectionWidget {
public:
	FollowRequestsSection(QWidget *parent, Window::SessionController *controller)
	: SectionWidget(parent, controller)
	, _bar(new Ui::TopBar(this))
	, _scroll(new Ui::ScrollArea(this)) {
		_bar->setTitle(tr::lng_eli_follow_requests(tr::now));
		setTopBar(_bar);
		_content = _scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(_scroll));
		load();
	}

	QString sectionKey() const override {
		return Routes::FollowRequests();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto area = QRect(0, _bar->height(), width(), std::max(height() - _bar->height(), 0));
		_scroll->setGeometry(area);
		if (_placeholder) {
			_placeholder->setGeometry(area);
		}
		if (_skeleton) {
			const auto column = std::min(width(), int(st::eliCardMaxWidth));
			_skeleton->setGeometry((width() - column) / 2, area.y(), column, area.height());
		}
	}

private:
	void load() {
		delete _placeholder;
		_placeholder = nullptr;
		_skeleton = new Ui::SkeletonList(this, Ui::SkeletonList::Kind::Chats, 4);
		_skeleton->show();
		_scroll->hide();
		resizeEvent(nullptr);
		controller()->session().data().graph().followRequests(
			base::Guard(this),
			[this](const QVector<Data::Profile> &users) { show(users); },
			[this](const Api::Error &error) {
				delete _skeleton;
				_skeleton = nullptr;
				auto descriptor = Ui::EmptyPlaceholderDescriptor();
				descriptor.icon = &st::emptyIconError;
				descriptor.title = tr::lng_error_load(tr::now);
				descriptor.text = Support::ErrorText(error);
				descriptor.buttonText = tr::lng_try_again(tr::now);
				descriptor.buttonCallback = [this] {
					Ui::PostponeCall(this, [this] { load(); });
				};
				_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
				_placeholder->show();
				resizeEvent(nullptr);
			});
	}

	void showEmpty() {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		descriptor.icon = &st::eliEmptyIconRequest;
		descriptor.title = tr::lng_eli_no_new_requests(tr::now);
		_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
		_placeholder->show();
		_scroll->hide();
		resizeEvent(nullptr);
	}

	void show(const QVector<Data::Profile> &users) {
		delete _skeleton;
		_skeleton = nullptr;
		if (users.isEmpty()) {
			showEmpty();
			return;
		}
		_scroll->show();
		const auto about = _content->add(
			Ui::object_ptr<Ui::FlatLabel>(
				_content,
				tr::lng_eli_requests_about(tr::now),
				st::captionLabel),
			st::eliRequestAboutPadding);
		Q_UNUSED(about);
		_left = int(users.size());
		for (const auto &user : users) {
			const auto row = _content->add(
				Ui::object_ptr<RequestRow>(_content, controller(), user));
			row->removed = [=, this] {
				Ui::PostponeCall(this, [=, this] {
					_content->removeChild(row);
					if (--_left <= 0) {
						showEmpty();
					}
				});
			};
		}
		resizeEvent(nullptr);
	}

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	int _left = 0;

};

} // namespace

Window::SectionFactory NotificationsFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new NotificationsSection(parent, controller));
	};
}

Window::SectionFactory FollowRequestsFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new FollowRequestsSection(parent, controller));
	};
}

} // namespace Eli
