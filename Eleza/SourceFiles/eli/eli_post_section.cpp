#include "eli/eli_post_section.h"

#include "api/api_client_config.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "eli/eli_actions.h"
#include "eli/eli_logic.h"
#include "eli/eli_post_list.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtCore/QUuid>
#include <QtGui/QKeyEvent>

namespace Eli {
namespace {

constexpr auto kMaxHops = 8;
constexpr auto kFoldAfter = 3;
constexpr auto kDefaultReplyLength = 500;

[[nodiscard]] int ColumnWidth(int width) {
	return std::min(width, int(st::eliCardMaxWidth));
}

} // namespace

// «N پاسخ» and the sort order.
class PostSection::RepliesHeader final : public Ui::RpWidget {
public:
	explicit RepliesHeader(QWidget *parent) : RpWidget(parent) {
		setMouseTracking(true);
	}

	void setTitle(const QString &title) {
		_title = title;
		update();
	}
	void setSort(const QString &sort) {
		_sort = sort;
		update();
	}
	void setSortClicked(Fn<void(QPoint global)> callback) {
		_clicked = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::eliRepliesHeaderHeight;
	}

	[[nodiscard]] QRect sortRect() const {
		const auto column = ColumnWidth(width());
		const auto left = (width() - column) / 2;
		const auto sortWidth = st::eliRepliesSortFont->width(_sort)
			+ 4
			+ st::eliRepliesSortIcon.width();
		return style::rtlrect(
			left + column - st::eliDetailPadding.right() - sortWidth,
			0,
			sortWidth,
			height(),
			width());
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto column = ColumnWidth(width());
		const auto left = (width() - column) / 2;
		p.setPen(st::eliRepliesHeaderFg);
		p.setFont(st::eliRepliesHeaderFont);
		p.drawTextLeft(
			left + st::eliDetailPadding.left(),
			(height() - st::eliRepliesHeaderFont->height) / 2,
			width(),
			_title);
		const auto sortWidth = st::eliRepliesSortFont->width(_sort);
		const auto x = left
			+ column
			- st::eliDetailPadding.right()
			- sortWidth
			- 4
			- st::eliRepliesSortIcon.width();
		p.setPen(st::eliRepliesSortFg);
		p.setFont(_over
			? st::eliRepliesSortFont->underline()
			: st::eliRepliesSortFont);
		p.drawTextLeft(
			x,
			(height() - st::eliRepliesSortFont->height) / 2,
			width(),
			_sort,
			sortWidth);
		st::eliRepliesSortIcon.paint(
			p,
			x + sortWidth + 4,
			(height() - st::eliRepliesSortIcon.height()) / 2 + 1,
			width());
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = sortRect().contains(e->pos());
		if (over != _over) {
			_over = over;
			setCursor(over ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		if (_over) {
			_over = false;
			setCursor(style::cur_default);
			update();
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && sortRect().contains(e->pos()) && _clicked) {
			const auto rect = sortRect();
			_clicked(mapToGlobal(style::RightToLeft()
				? QPoint(rect.right() + 1, rect.bottom() - 8)
				: QPoint(rect.left(), rect.bottom() - 8)));
		}
	}

private:
	QString _title;
	QString _sort;
	Fn<void(QPoint)> _clicked;
	bool _over = false;

};

// What follows the replies: a spinner, «be the first», an error with a retry.
class PostSection::RepliesTail final : public Ui::RpWidget {
public:
	enum class State {
		Idle,
		Loading,
		Empty,
		Failed,
	};

	explicit RepliesTail(QWidget *parent)
	: RpWidget(parent)
	, _spinner(new Ui::LoadingSpinner(this, 28, st::brandPrimary))
	, _retry(new Ui::LinkButton(this, tr::lng_retry(tr::now))) {
		_spinner->hide();
		_retry->hide();
	}

	void setState(State state, const QString &text = QString()) {
		_state = state;
		_text = text;
		_spinner->setVisible(state == State::Loading);
		_retry->setVisible(state == State::Failed);
		resizeToWidth(width());
		update();
	}
	void setRetry(Fn<void()> callback) {
		_retry->setClickedCallback(std::move(callback));
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto height = (_state == State::Idle)
			? int(st::eliListBottomSkip)
			: int(st::eliRepliesTailHeight);
		_spinner->move((newWidth - _spinner->width()) / 2, (height - _spinner->height()) / 2);
		_retry->resizeToNaturalWidth(newWidth);
		_retry->move((newWidth - _retry->width()) / 2, height / 2 + 6);
		return height;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_state != State::Empty && _state != State::Failed) {
			return;
		}
		auto p = Painter(this);
		p.setPen(st::eliRepliesEmptyFg);
		p.setFont(st::eliRepliesEmptyFont);
		const auto text = st::eliRepliesEmptyFont->elided(_text, width() - 2 * st::spaceLg);
		const auto top = (_state == State::Failed)
			? (height() / 2 - st::eliRepliesEmptyFont->height - 2)
			: ((height() - st::eliRepliesEmptyFont->height) / 2);
		p.drawTextCentered(QRect(0, top, width(), st::eliRepliesEmptyFont->height), text);
	}

private:
	State _state = State::Idle;
	QString _text;
	Ui::LoadingSpinner *_spinner = nullptr;
	Ui::LinkButton *_retry = nullptr;

};

// The quick reply field docked under the conversation.
class PostSection::ReplyDock final : public Ui::RpWidget {
public:
	ReplyDock(QWidget *parent, int maxLength)
	: RpWidget(parent)
	, _field(new Ui::InputField(
		this,
		st::eliDockField,
		Ui::InputField::Mode::MultiLine))
	, _expand(new Ui::IconButton(this, st::eliDockExpand))
	, _send(new Ui::IconButton(this, st::eliDockSend))
	, _cancelTarget(new Ui::IconButton(this, st::eliDockTargetClose)) {
		_field->setMaxLength(maxLength);
		_field->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
		connect(_field, &Ui::RpWidget::heightChanged, this, [this] {
			resizeToWidth(width());
		});
		connect(_field, &Ui::InputField::changed, this, [this] {
			refreshSend();
		});
		connect(_field, &Ui::InputField::submitted, this, [this] {
			if (submit) {
				submit();
			}
		});
		connect(_field, &Ui::InputField::cancelled, this, [this] {
			if (cancelled) {
				cancelled();
			}
		});
		_send->setClickedCallback([this] {
			if (submit) {
				submit();
			}
		});
		_expand->setClickedCallback([this] {
			if (expand) {
				expand();
			}
		});
		_cancelTarget->setClickedCallback([this] {
			if (cancelTarget) {
				cancelTarget();
			}
		});
		_cancelTarget->hide();
		Ui::InstallTooltip(_expand, [] { return tr::lng_eli_full_reply(tr::now); });
		Ui::InstallTooltip(_send, [] { return tr::lng_send(tr::now); });
		refreshSend();
	}

	Fn<void()> submit;
	Fn<void()> expand;
	Fn<void()> cancelled;
	Fn<void()> cancelTarget;

	void setPlaceholder(const QString &text) {
		_field->setPlaceholder(text);
	}
	void setTarget(const QString &text) {
		if (_target == text) {
			return;
		}
		_target = text;
		_cancelTarget->setVisible(!text.isEmpty() && !_closed);
		resizeToWidth(width());
		update();
	}
	void setClosed(bool closed) {
		if (_closed == closed) {
			return;
		}
		_closed = closed;
		_field->setVisible(!closed);
		_expand->setVisible(!closed);
		_send->setVisible(!closed);
		_cancelTarget->setVisible(!closed && !_target.isEmpty());
		resizeToWidth(width());
		update();
	}
	void setSending(bool sending) {
		_sending = sending;
		_field->setReadOnly(sending);
		refreshSend();
	}
	[[nodiscard]] QString text() const {
		return _field->text().trimmed();
	}
	void clear() {
		_field->clear();
	}
	void setFocusFast() {
		if (!_closed) {
			_field->setFocusFast();
		}
	}
	[[nodiscard]] bool fieldFocused() const {
		return _field->hasFocus();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto column = ColumnWidth(newWidth);
		_left = (newWidth - column) / 2;
		_column = column;
		const auto &padding = st::eliDockPadding;
		if (_closed) {
			return st::eliDockClosedHeight;
		}
		auto top = padding.top();
		if (!_target.isEmpty()) {
			_cancelTarget->moveToRight(
				_left + padding.right(),
				top + (st::eliDockTargetHeight - _cancelTarget->height()) / 2,
				newWidth);
			top += st::eliDockTargetHeight;
		}
		const auto buttons = _send->width() + _expand->width();
		const auto fieldWidth = column - padding.left() - padding.right() - buttons - 4;
		_field->resizeToWidth(std::max(fieldWidth, 40));
		_field->moveToLeft(_left + padding.left(), top, newWidth);
		_fieldRect = QRect(
			_left + padding.left(),
			top,
			column - padding.left() - padding.right(),
			_field->height());
		const auto buttonTop = top + _field->height() - _send->height();
		_send->moveToRight(_left + padding.right(), buttonTop, newWidth);
		_expand->moveToRight(_left + padding.right() + _send->width(), buttonTop, newWidth);
		return top + _field->height() + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(rect(), st::eliDockBg);
		p.fillRect(QRect(0, 0, width(), 1), st::eliDockLineFg);
		if (_closed) {
			p.setPen(st::eliDockClosedFg);
			p.setFont(st::eliDockClosedFont);
			p.drawTextCentered(rect(), tr::lng_eli_replies_are_closed(tr::now));
			return;
		}
		if (!_target.isEmpty()) {
			p.setPen(st::eliDockTargetFg);
			p.setFont(st::eliDockTargetFont);
			const auto &padding = st::eliDockPadding;
			const auto available = _column - padding.left() - padding.right() - _cancelTarget->width() - 8;
			p.drawTextLeft(
				_left + padding.left() + 14,
				padding.top() + (st::eliDockTargetHeight - st::eliDockTargetFont->height) / 2,
				width(),
				st::eliDockTargetFont->elided(_target, std::max(available, 0)));
		}
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::eliDockFieldBg);
		p.drawRoundedRect(
			style::rtlrect(_fieldRect, width()),
			st::eliDockFieldRadius,
			st::eliDockFieldRadius);
	}

private:
	void refreshSend() {
		const auto enabled = !_sending && !_field->text().trimmed().isEmpty();
		_send->setDisabled(!enabled);
		_send->setIconOverride(
			enabled ? nullptr : &st::eliDockSendDisabled,
			enabled ? nullptr : &st::eliDockSendDisabled);
	}

	Ui::InputField *_field = nullptr;
	Ui::IconButton *_expand = nullptr;
	Ui::IconButton *_send = nullptr;
	Ui::IconButton *_cancelTarget = nullptr;
	QString _target;
	QRect _fieldRect;
	int _left = 0;
	int _column = 0;
	bool _closed = false;
	bool _sending = false;

};

PostSection::PostSection(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &postId,
	const QString &rootHint)
: SectionWidget(parent, controller)
, _requestedId(postId)
, _rootHint((rootHint != postId) ? rootHint : QString())
, _bar(new Ui::TopBar(this))
, _scroll(new Ui::ScrollArea(this)) {
	_bar->setTitle(tr::lng_eli_post_title(tr::now));
	setTopBar(_bar);
	_content = _scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(_scroll));
	_scroll->setNearEdgeThreshold(600);
	connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		if (_repliesLoadedOnce && !_repliesFailed) {
			loadReplies();
		}
	});

	auto &data = controller->session().data();
	connect(&data.feed(), &Data::Feed::postChanged, this, [this](const QString &id) {
		if (id == _rootId) {
			refreshHeader();
			refreshDock();
		}
	});
	connect(&data.feed(), &Data::Feed::postRemoved, this, [this](const QString &id) {
		if (_replyIds.contains(id)) {
			arrange();
			refreshTail();
		}
	});
	connect(&data.feed(), &Data::Feed::postPublished, this, [this](const QString &id) {
		if (_rootId.isEmpty() || _replyIds.contains(id)) {
			return;
		}
		const auto post = this->controller()->session().data().feed().post(id);
		if (!post || post->replyTo.isEmpty()) {
			return;
		}
		if (post->replyTo == _rootId) {
			_replyIds.push_back(id);
			_mineFirst.push_front(id);
		} else if (_replyIds.contains(post->replyTo)) {
			_replyIds.push_back(id);
			// Open the branch it landed in.
			auto parent = post->replyTo;
			for (auto hop = 0; hop != kMaxHops; ++hop) {
				const auto found = this->controller()->session().data().feed().post(parent);
				if (!found || found->replyTo.isEmpty() || found->replyTo == _rootId) {
					break;
				}
				parent = found->replyTo;
			}
			_expandedBranches.insert(parent);
		} else {
			return;
		}
		arrange();
		refreshTail();
		_highlightId = id;
		applyHighlight();
	});
	connect(&data.graph(), &Data::Graph::relationshipChanged, this, [this](
			const QString &userId,
			const Data::GraphState &state) {
		if (!_root || _rootId.isEmpty()) {
			return;
		}
		const auto card = _root->card(_rootId);
		if (card && card->post().author == userId) {
			card->setFollowState(state.follow.isEmpty() ? u"none"_s : state.follow);
			_root->resizeToWidth(_root->width());
			_root->update();
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this] {
		refreshDock();
	});
	load();
}

QString PostSection::sectionKey() const {
	return Window::Routes::Post(_requestedId);
}

void PostSection::setInnerFocus() {
	if (_replies) {
		_replies->setFocus();
	} else {
		setFocus();
	}
}

bool PostSection::onBackRequest() {
	if (!_replyTarget.isEmpty() && _replyTarget != _rootId && _dock && _dock->fieldFocused()) {
		setReplyTarget(_rootId);
		return true;
	}
	return false;
}

void PostSection::load() {
	++_generation;
	if (_error) {
		delete _error;
		_error = nullptr;
	}
	if (!_skeleton) {
		_skeleton = new Ui::SkeletonList(this, Ui::SkeletonList::Kind::Posts, 3);
		_skeleton->show();
	}
	_scroll->hide();
	updateLayout();
	_climb.clear();
	_afterHint = nullptr;
	loadHint();
	const auto generation = _generation;
	controller()->session().data().feed().fetchPost(
		_requestedId,
		base::Guard(this),
		[=, this](const Data::Post &post) {
			if (generation == _generation) {
				resolve(post, 0);
			}
		},
		[=, this](const Api::Error &error) {
			if (generation == _generation) {
				requestedFailed(error);
			}
		});
}

// The post a notification named as the top of the conversation is read at
// the same time as the comment.
void PostSection::loadHint() {
	if (_rootHint.isEmpty()) {
		_hintState = HintState::None;
		return;
	}
	const auto known = controller()->session().data().feed().post(_rootHint);
	if (known && !known->author.isEmpty() && !known->deleted) {
		_hintState = HintState::Ready;
		return;
	}
	_hintState = HintState::Loading;
	const auto generation = _generation;
	const auto finished = [=, this](HintState state) {
		if (generation != _generation) {
			return;
		}
		_hintState = state;
		if (const auto then = std::exchange(_afterHint, nullptr)) {
			then();
		}
	};
	controller()->session().data().feed().fetchPost(
		_rootHint,
		base::Guard(this),
		[=](const Data::Post &) { finished(HintState::Ready); },
		[=](const Api::Error &) { finished(HintState::Failed); });
}

void PostSection::requestedFailed(const Api::Error &error) {
	// A comment that can't be read (gone, hidden): its post alone, when the
	// notification named it.
	switch (_hintState) {
	case HintState::Loading:
		_afterHint = [=, this] { requestedFailed(error); };
		return;
	case HintState::Ready:
		_climb.clear();
		showRoot(_rootHint);
		return;
	case HintState::None:
	case HintState::Failed:
		break;
	}
	showError(error);
}

void PostSection::resolve(const Data::Post &post, int hop) {
	auto &feed = controller()->session().data().feed();
	if (_climb.isEmpty() || _climb.back().id != post.id) {
		auto step = Logic::ClimbStep();
		step.id = post.id;
		step.comment = !post.replyTo.isEmpty();
		step.deleted = post.deleted;
		_climb.push_back(step);
	}
	auto next = QString();
	if (!post.replyTo.isEmpty() && hop < kMaxHops) {
		next = post.replyTo;
	} else if (!post.threadRoot.isEmpty() && post.threadRoot != post.id) {
		next = post.threadRoot;
		hop = kMaxHops; // the root of a thread is where it ends
	}
	if (next.isEmpty()) {
		showRoot(post.id);
		return;
	}
	if (next == _rootHint && _hintState == HintState::Loading) {
		_afterHint = [=, this] { resolve(post, hop); };
		return;
	}
	if (const auto known = feed.post(next); known && !known->author.isEmpty()) {
		resolve(Data::Post(*known), hop + 1);
		return;
	}
	const auto generation = _generation;
	const auto current = post.id;
	feed.fetchPost(
		next,
		base::Guard(this),
		[=, this](const Data::Post &parent) {
			if (generation == _generation) {
				resolve(parent, hop + 1);
			}
		},
		[=, this](const Api::Error &) {
			if (generation == _generation) {
				climbEnded(current);
			}
		});
}

// A parent that can't be read ends the climb: the post the notification
// named is the top when there is one, otherwise what we have is the top of
// what is left.
void PostSection::climbEnded(const QString &top) {
	if (_hintState == HintState::Loading) {
		_afterHint = [=, this] { climbEnded(top); };
		return;
	}
	const auto inClimb = std::any_of(_climb.cbegin(), _climb.cend(), [&](
			const Logic::ClimbStep &step) {
		return step.id == _rootHint;
	});
	if (_hintState == HintState::Ready && !inClimb) {
		showRoot(_rootHint);
	} else {
		showRoot(top);
	}
}

void PostSection::showError(const Api::Error &error) {
	if (_skeleton) {
		delete _skeleton;
		_skeleton = nullptr;
	}
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = &st::emptyIconEli;
	descriptor.title = tr::lng_eli_post_no_longer_available(tr::now);
	descriptor.text = error.notFound() ? QString() : Support::ErrorText(error);
	descriptor.buttonText = tr::lng_retry(tr::now);
	descriptor.buttonCallback = [this] {
		Ui::PostponeCall(this, [this] { load(); });
	};
	_error = new Ui::EmptyPlaceholder(this, std::move(descriptor));
	_error->show();
	updateLayout();
}

void PostSection::showRoot(const QString &rootId) {
	if (_skeleton) {
		delete _skeleton;
		_skeleton = nullptr;
	}
	_rootId = rootId;
	_replyTarget = rootId;
	// (a deleted comment: the post and its comments alone)
	_focusPath = Logic::FocusPath(_climb);
	_focusPath.removeAll(rootId);
	_highlightId = _focusPath.isEmpty() ? QString() : _focusPath.back();
	const auto controller = this->controller();
	auto &feed = controller->session().data().feed();
	const auto root = feed.post(rootId);
	const auto author = root ? root->author : QString();

	auto rootDescriptor = ListDescriptor();
	rootDescriptor.mode = CardMode::Detail;
	rootDescriptor.keepRemoved = true;
	rootDescriptor.replyToOwner = true;
	_root = _content->add(
		Ui::object_ptr<PostList>(_content, controller, std::move(rootDescriptor)));
	auto row = ListRow();
	row.id = rootId;
	row.options.mode = CardMode::Detail;
	row.options.expandThread = true;
	_root->setRows({ row });
	connect(_root, &PostList::replyRequested, this, [this](const QString &id) {
		setReplyTarget(id);
		_dock->setFocusFast();
	});
	connect(_root, &PostList::followRequested, this, [this] {
		toggleFollow();
	});

	_header = _content->add(Ui::object_ptr<RepliesHeader>(_content));
	_header->setSortClicked([this](QPoint global) {
		const auto menu = new Ui::PopupMenu(this);
		const auto add = [&](const QString &text, bool newest) {
			auto item = Ui::MenuItem();
			item.text = text;
			item.checked = (_newestFirst == newest);
			item.callback = [=, this] {
				if (_newestFirst != newest) {
					_newestFirst = newest;
					arrange();
					refreshHeader();
				}
			};
			menu->addItem(std::move(item));
		};
		add(tr::lng_eli_most_relevant(tr::now), false);
		add(tr::lng_eli_newest(tr::now), true);
		menu->popup(global);
	});

	auto repliesDescriptor = ListDescriptor();
	repliesDescriptor.mode = CardMode::Reply;
	repliesDescriptor.keepRemoved = true;
	repliesDescriptor.replyToOwner = true;
	_replies = _content->add(
		Ui::object_ptr<PostList>(_content, controller, std::move(repliesDescriptor)));
	connect(_replies, &PostList::replyRequested, this, [this](const QString &id) {
		setReplyTarget(id);
		_dock->setFocusFast();
	});
	connect(_replies, &PostList::moreRequested, this, [this](const QString &key) {
		if (key == u"page") {
			loadReplies();
		} else if (key.startsWith(u"more:")) {
			_expandedBranches.insert(key.mid(5));
			arrange();
		}
	});
	connect(_replies, &PostList::scrollToRequested, this, [this](int top, int bottom) {
		const auto shift = _replies->y();
		_scroll->scrollToY(top + shift, bottom + shift);
	});

	_tail = _content->add(Ui::object_ptr<RepliesTail>(_content));
	_tail->setRetry([this] {
		_repliesFailed = false;
		loadReplies();
	});

	auto limit = controller->session().account().clientConfig().postMaxTextChars();
	if (limit <= 0) {
		limit = kDefaultReplyLength;
	}
	_dock = new ReplyDock(this, limit);
	_dock->submit = [this] { sendReply(); };
	_dock->cancelled = [this] {
		if (_replyTarget != _rootId) {
			setReplyTarget(_rootId);
		} else {
			this->controller()->showBack();
		}
	};
	_dock->cancelTarget = [this] { setReplyTarget(_rootId); };
	_dock->expand = [this] {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Reply;
		args.postId = _replyTarget;
		args.prefill = _dock->text();
		_dock->clear();
		OpenCompose(this->controller(), args);
	};
	connect(_dock, &Ui::RpWidget::heightChanged, this, [this] {
		updateLayout();
	});
	_dock->show();

	_scroll->show();
	refreshHeader();
	refreshDock();
	updateLayout();
	loadReplies();

	if (!author.isEmpty() && author != controller->session().userId()) {
		controller->session().data().graph().relationship(
			author,
			base::Guard(this),
			[this](const Data::GraphState &state) {
				if (const auto card = _root->card(_rootId)) {
					card->setFollowState(state.follow.isEmpty() ? u"none"_s : state.follow);
					_root->resizeToWidth(_root->width());
					_root->update();
				}
			});
	}
	setInnerFocus();
}

void PostSection::toggleFollow() {
	const auto card = _root ? _root->card(_rootId) : nullptr;
	if (!card) {
		return;
	}
	const auto author = card->post().author;
	const auto following = (card->followState() != u"none");
	auto &graph = controller()->session().data().graph();
	const auto fail = [this](const Api::Error &error) {
		Support::ShowError(this->controller(), error);
	};
	// The answer comes back through relationshipChanged as well.
	const auto done = [this](const Data::GraphState &state) {
		if (const auto card = _root->card(_rootId)) {
			card->setFollowState(state.follow.isEmpty() ? u"none"_s : state.follow);
			_root->resizeToWidth(_root->width());
			_root->update();
		}
	};
	if (following) {
		graph.unfollow(author, base::Guard(this), done, fail);
	} else {
		graph.follow(author, base::Guard(this), done, fail);
	}
}

void PostSection::loadReplies() {
	if (_repliesLoading || _repliesDone || _rootId.isEmpty()) {
		return;
	}
	_repliesLoading = true;
	_repliesFailed = false;
	refreshTail();
	const auto generation = _generation;
	controller()->session().data().feed().replies(
		_rootId,
		_cursor,
		base::Guard(this),
		[=, this](const Data::RepliesPage &page) {
			if (generation != _generation) {
				return;
			}
			_repliesLoading = false;
			_repliesLoadedOnce = true;
			auto ids = page.replies;
			if (!_focusPlaced && !_focusPath.isEmpty()) {
				_focusPlaced = true;
				// The opened comment's thread first and open, unless the
				// page brings it (its posts were read on the climb).
				ids = Logic::PinThread(ids, _focusPath);
				if (ids != page.replies) {
					_pinnedTop = _focusPath.front();
				}
				_expandedBranches.insert(_focusPath.front());
			}
			for (const auto &id : ids) {
				if (!_replyIds.contains(id)) {
					_replyIds.push_back(id);
				}
			}
			_cursor = page.nextCursor;
			_repliesDone = _cursor.isEmpty() || page.replies.isEmpty();
			arrange();
			refreshTail();
			applyHighlight();
		},
		[=, this](const Api::Error &error) {
			if (generation != _generation) {
				return;
			}
			_repliesLoading = false;
			_repliesLoadedOnce = true;
			_repliesFailed = true;
			_tail->setState(
				RepliesTail::State::Failed,
				Support::ErrorText(error, tr::lng_eli_replies_failed(tr::now)));
		});
}

void PostSection::arrange() {
	if (!_replies) {
		return;
	}
	auto &session = controller()->session();
	auto &feed = session.data().feed();
	auto nodes = QVector<Logic::ReplyNode>();
	for (const auto &id : _replyIds) {
		const auto post = feed.post(id);
		if (!post) {
			continue;
		}
		auto node = Logic::ReplyNode();
		node.id = id;
		node.replyTo = (post->replyTo == _rootId) ? QString() : post->replyTo;
		node.deleted = post->deleted;
		nodes.push_back(node);
	}
	auto first = _mineFirst;
	if (!_pinnedTop.isEmpty() && !first.contains(_pinnedTop)) {
		first.push_back(_pinnedTop);
	}
	const auto arranged = Logic::ArrangeReplies(
		nodes,
		_rootId,
		_newestFirst,
		first);
	auto rows = QVector<ListRow>();
	for (auto i = 0; i != int(arranged.size());) {
		const auto &top = arranged[i];
		// [i + 1, end) hangs under this top-level reply.
		auto end = i + 1;
		while (end != int(arranged.size()) && arranged[end].depth == 1) {
			++end;
		}
		const auto nested = end - i - 1;
		const auto fold = !_expandedBranches.contains(top.id)
			&& (nested > kFoldAfter + 1);
		const auto shown = fold ? kFoldAfter : nested;
		for (auto j = i; j != i + 1 + shown; ++j) {
			const auto &entry = arranged[j];
			auto row = ListRow();
			row.id = entry.id;
			row.options.mode = CardMode::Reply;
			row.options.depth = entry.depth;
			row.options.line = entry.line || (fold && j == i + shown);
			if (!entry.parent.isEmpty()) {
				if (const auto parent = feed.post(entry.parent)) {
					row.options.replyingTo = parent->author;
				}
			}
			rows.push_back(row);
		}
		if (fold) {
			auto row = ListRow();
			row.id = u"more:"_s + top.id;
			row.options.depth = 1;
			row.moreText = tr::lng_eli_show_n_replies(
				tr::now,
				lt_count,
				double(nested - shown));
			rows.push_back(row);
		}
		i = end;
	}
	if (!_repliesDone && !rows.isEmpty()) {
		auto row = ListRow();
		row.id = u"page"_s;
		row.moreText = tr::lng_eli_show_more_replies(tr::now);
		rows.push_back(row);
	}
	_replies->setRows(rows);
}

void PostSection::applyHighlight() {
	if (_highlightId.isEmpty() || !_replies) {
		return;
	}
	const auto id = _highlightId;
	if (!_replyIds.contains(id)) {
		// Not read yet: it was pinned in front when it could be (the list
		// is not read ahead for it).
		if (_repliesLoadedOnce) {
			_highlightId = QString();
		}
		return;
	}
	if (_replies->rowRect(id).isEmpty()) {
		// Folded: open the branch it is in.
		auto &feed = controller()->session().data().feed();
		auto parent = id;
		for (auto hop = 0; hop != kMaxHops; ++hop) {
			const auto found = feed.post(parent);
			if (!found || found->replyTo.isEmpty() || found->replyTo == _rootId) {
				break;
			}
			parent = found->replyTo;
		}
		_expandedBranches.insert(parent);
		arrange();
	}
	_highlightId = QString();
	Ui::PostponeCall(this, [=, this] {
		const auto rect = _replies->rowRect(id);
		if (rect.isEmpty()) {
			return;
		}
		const auto top = _replies->y() + rect.y();
		_scroll->scrollToY(std::max(top - _scroll->height() / 3, 0));
		_replies->highlight(id);
	});
}

void PostSection::refreshHeader() {
	if (!_header) {
		return;
	}
	const auto root = controller()->session().data().feed().post(_rootId);
	const auto count = root ? root->counts.replies : 0;
	_header->setTitle((count > 0)
		? tr::lng_eli_replies_count(tr::now, lt_count, double(count))
		: tr::lng_eli_replies(tr::now));
	_header->setSort(_newestFirst
		? tr::lng_eli_newest(tr::now)
		: tr::lng_eli_most_relevant(tr::now));
}

void PostSection::refreshTail() {
	if (!_tail || !_replies) {
		return;
	}
	if (_repliesFailed) {
		return;
	} else if (_repliesLoading && _replies->count() == 0) {
		_tail->setState(RepliesTail::State::Loading);
	} else if (_replies->count() == 0 && _repliesLoadedOnce) {
		const auto root = controller()->session().data().feed().post(_rootId);
		_tail->setState(
			RepliesTail::State::Empty,
			(root && !root->canReply)
				? tr::lng_eli_replies_are_closed(tr::now)
				: tr::lng_eli_no_replies_yet_be_first(tr::now));
	} else if (_repliesLoading) {
		_tail->setState(RepliesTail::State::Loading);
	} else {
		_tail->setState(RepliesTail::State::Idle);
	}
}

void PostSection::refreshDock() {
	if (!_dock) {
		return;
	}
	auto &session = controller()->session();
	const auto root = session.data().feed().post(_rootId);
	const auto target = session.data().feed().post(_replyTarget);
	_dock->setClosed(!root || root->deleted || !root->canReply);
	const auto name = AuthorName(
		session,
		target ? target->author : (root ? root->author : QString()));
	_dock->setPlaceholder(tr::lng_eli_reply_to_placeholder(tr::now, lt_name, name));
	_dock->setTarget((_replyTarget != _rootId && target)
		? tr::lng_eli_replying_to_name(tr::now, lt_name, name)
		: QString());
}

void PostSection::setReplyTarget(const QString &postId) {
	_replyTarget = postId.isEmpty() ? _rootId : postId;
	refreshDock();
}

void PostSection::sendReply() {
	const auto text = _dock ? _dock->text() : QString();
	if (text.isEmpty() || _sending) {
		return;
	}
	_sending = true;
	_dock->setSending(true);
	auto body = Data::CreatePost();
	body.type = u"text"_s;
	body.text = text;
	body.replyTo = _replyTarget;
	body.replyPolicy = u"everyone"_s;
	const auto controller = this->controller();
	const auto target = _replyTarget;
	controller->session().data().feed().createPost(
		body,
		QUuid::createUuid().toString(QUuid::WithoutBraces),
		base::Guard(this),
		[=, this](const Data::Post &created) {
			_sending = false;
			_dock->setSending(false);
			_dock->clear();
			setReplyTarget(_rootId);
			// Not every answer repeats what the post replies to.
			auto reply = created;
			if (reply.replyTo.isEmpty()) {
				reply.replyTo = target;
			}
			BumpReplies(controller, reply);
			controller->showToast(tr::lng_eli_reply_was_sent(tr::now));
		},
		[=, this](const Api::Error &error) {
			_sending = false;
			_dock->setSending(false);
			Support::ShowError(controller, error, tr::lng_eli_reply_failed(tr::now));
		});
}

void PostSection::updateLayout() {
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	auto bottom = height();
	if (_dock) {
		_dock->resizeToWidth(width());
		bottom -= _dock->height();
		_dock->move(0, bottom);
	}
	const auto area = QRect(0, top, width(), std::max(bottom - top, 0));
	_scroll->setGeometry(area);
	if (_skeleton) {
		const auto column = ColumnWidth(width());
		_skeleton->setGeometry((width() - column) / 2, top, column, area.height());
	}
	if (_error) {
		_error->setGeometry(area);
	}
}

void PostSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateLayout();
}

int PostSection::toastBottomSkip() const {
	return (_dock && !_dock->isHidden()) ? _dock->height() : 0;
}

void PostSection::keyPressEvent(QKeyEvent *e) {
	if (_replies && _replies->handleKey(e)) {
		return;
	}
	SectionWidget::keyPressEvent(e);
}

// The count of replies of the posts above a freshly published reply (the
// server tells it with the next payload; until then the cards would lag).
void BumpReplies(Window::SessionController *controller, const Data::Post &created) {
	if (created.replyTo.isEmpty()) {
		return;
	}
	auto &feed = controller->session().data().feed();
	// A local change: a plain merge would lose the count while another
	// optimistic change of the same post (a like) is still held.
	feed.noteReplyAdded(created.replyTo);
	if (!created.threadRoot.isEmpty()
		&& created.threadRoot != created.id
		&& created.threadRoot != created.replyTo) {
		feed.noteReplyAdded(created.threadRoot);
	}
}

// ---- thread reader

class ThreadSection::Progress final : public Ui::RpWidget {
public:
	explicit Progress(QWidget *parent) : RpWidget(parent) {
		Ui::MakeTransparentForMouse(this);
	}

	void setProgress(double progress) {
		_progress = std::clamp(progress, 0., 1.);
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		const auto done = int(width() * _progress);
		p.fillRect(
			style::rtlrect(0, 0, done, height(), width()),
			st::eliThreadProgressFg);
	}

private:
	double _progress = 0.;

};

class ThreadSection::Content final : public Ui::RpWidget {
public:
	Content(
		QWidget *parent,
		Window::SessionController *controller,
		const Data::Post &post)
	: RpWidget(parent)
	, _controller(controller)
	, _author(post.author)
	, _userpic(new Support::UserpicButton(
		this,
		&controller->session(),
		post.author,
		st::eliCardUserpic))
	, _open(new Ui::RoundButton(
		this,
		tr::lng_eli_see_post(tr::now),
		st::outlineRoundButton)) {
		auto texts = QStringList{ post.text };
		for (const auto &part : post.threadParts) {
			texts.push_back(part.text);
		}
		for (const auto &text : texts) {
			const auto label = new Ui::FlatLabel(this, st::eliThreadReaderLabel);
			label->setAutoDirection(true);
			label->setMarkedText(PostText(text));
			label->setSelectable(true);
			label->setLinkHandler(Support::LinkHandler(controller));
			_parts.push_back(label);
		}
		_meta = tr::lng_eli_thread_meta(
			tr::now,
			lt_parts,
			Lang::Num(texts.size()),
			lt_minutes,
			Lang::Num(Logic::ReadingMinutes(texts)));
		const auto id = post.id;
		_userpic->setClickedCallback([=] { OpenProfile(controller, post.author); });
		_open->setClickedCallback([=] { OpenPost(controller, id); });
		connect(
			&controller->session().data().users(),
			&Data::Users::changed,
			this,
			[this] { update(); });
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::eliThreadPadding;
		_column = std::min(newWidth, int(st::eliThreadWidth));
		_left = (newWidth - _column) / 2;
		const auto inner = _column - padding.left() - padding.right();
		auto y = padding.top();
		_userpic->moveToLeft(
			_left + padding.left(),
			y + (st::eliThreadHeaderHeight - st::eliCardUserpic) / 2,
			newWidth);
		y += st::eliThreadHeaderHeight;
		_numberTops.clear();
		for (const auto label : _parts) {
			y += st::eliThreadPartSkipLarge;
			_numberTops.push_back(y);
			y += st::eliThreadPartNumberHeight;
			label->resizeToWidth(inner);
			label->moveToLeft(_left + padding.left(), y, newWidth);
			y += label->height();
		}
		y += st::eliThreadFooterSkip;
		_open->resizeToNaturalWidth(inner);
		_open->move(_left + (_column - _open->width()) / 2, y);
		y += _open->height() + padding.bottom();
		return y;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::eliThreadPadding;
		auto &session = _controller->session();
		const auto textLeft = _left
			+ padding.left()
			+ st::eliCardUserpic
			+ st::eliCardUserpicSkip;
		const auto top = padding.top()
			+ (st::eliThreadHeaderHeight - st::eliThreadNameFont->height - st::eliThreadMetaFont->height - 2) / 2;
		p.setPen(st::eliNameFg);
		p.setFont(st::eliThreadNameFont);
		p.drawTextLeft(textLeft, top, width(), AuthorName(session, _author));
		const auto user = session.data().users().get(_author);
		const auto meta = ((user.handle.isEmpty() || user.deleted)
			? QString()
			: (IsolatedHandle(user.handle) + tr::lng_eli_meta_separator(tr::now)))
			+ _meta;
		p.setPen(st::eliMetaFg);
		p.setFont(st::eliThreadMetaFont);
		p.drawTextLeft(textLeft, top + st::eliThreadNameFont->height + 2, width(), meta);
		const auto inner = _column - padding.left() - padding.right();
		for (auto i = 0; i != int(_numberTops.size()); ++i) {
			const auto y = _numberTops[i];
			const auto number = Lang::Num(i + 1);
			const auto numberWidth = st::eliThreadPartNumberFont->width(number);
			p.setPen(st::eliThreadPartNumberFg);
			p.setFont(st::eliThreadPartNumberFont);
			p.drawTextLeft(_left + padding.left(), y, width(), number, numberWidth);
			p.fillRect(
				style::rtlrect(
					_left + padding.left() + numberWidth + 10,
					y + st::eliThreadPartNumberFont->height / 2,
					inner - numberWidth - 10,
					1,
					width()),
				st::eliCardLineFg);
		}
	}

private:
	Window::SessionController *_controller = nullptr;
	QString _author;
	Support::UserpicButton *_userpic = nullptr;
	Ui::RoundButton *_open = nullptr;
	std::vector<Ui::FlatLabel*> _parts;
	std::vector<int> _numberTops;
	QString _meta;
	int _column = 0;
	int _left = 0;

};

ThreadSection::ThreadSection(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &postId)
: SectionWidget(parent, controller)
, _postId(postId)
, _bar(new Ui::TopBar(this))
, _progress(new Progress(this))
, _scroll(new Ui::ScrollArea(this)) {
	_bar->setTitle(tr::lng_eli_thread_title(tr::now));
	setTopBar(_bar);
	const auto refresh = [this] {
		const auto max = _scroll->scrollTopMax();
		_progress->setProgress((max > 0) ? (double(_scroll->scrollTop()) / max) : 0.);
	};
	connect(_scroll, &Ui::ScrollArea::scrolled, this, refresh);
	connect(_scroll, &Ui::ScrollArea::innerResized, this, refresh);
	load();
}

QString ThreadSection::sectionKey() const {
	return Routes::Thread(_postId);
}

void ThreadSection::setInnerFocus() {
	_scroll->setFocus();
}

void ThreadSection::load() {
	if (_error) {
		delete _error;
		_error = nullptr;
	}
	if (!_skeleton) {
		_skeleton = new Ui::SkeletonList(this, Ui::SkeletonList::Kind::Lines);
		_skeleton->show();
	}
	resizeEvent(nullptr);
	const auto fail = [this](const Api::Error &error) {
		delete _skeleton;
		_skeleton = nullptr;
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		descriptor.icon = &st::eliEmptyIconThread;
		descriptor.title = tr::lng_eli_post_no_longer_available(tr::now);
		descriptor.text = error.notFound() ? QString() : Support::ErrorText(error);
		descriptor.buttonText = tr::lng_retry(tr::now);
		descriptor.buttonCallback = [this] {
			Ui::PostponeCall(this, [this] { load(); });
		};
		_error = new Ui::EmptyPlaceholder(this, std::move(descriptor));
		_error->show();
		resizeEvent(nullptr);
	};
	auto &feed = controller()->session().data().feed();
	feed.fetchPost(_postId, base::Guard(this), [=, this](const Data::Post &post) {
		if (!post.threadRoot.isEmpty() && post.threadRoot != post.id) {
			controller()->session().data().feed().fetchPost(
				post.threadRoot,
				base::Guard(this),
				[this](const Data::Post &root) { show(root); },
				fail);
		} else {
			show(post);
		}
	}, fail);
}

void ThreadSection::show(const Data::Post &post) {
	delete _skeleton;
	_skeleton = nullptr;
	_bar->setTitle(tr::lng_eli_thread_of(
		tr::now,
		lt_name,
		AuthorName(controller()->session(), post.author)));
	_content = _scroll->setOwnedWidget(
		Ui::object_ptr<Content>(_scroll, controller(), post));
	resizeEvent(nullptr);
}

void ThreadSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	_progress->setGeometry(
		0,
		top - st::eliThreadProgressHeight,
		width(),
		st::eliThreadProgressHeight);
	_progress->raise();
	const auto area = QRect(0, top, width(), std::max(height() - top, 0));
	_scroll->setGeometry(area);
	if (_skeleton) {
		const auto column = std::min(width(), int(st::eliThreadWidth));
		_skeleton->setGeometry((width() - column) / 2, top, column, area.height());
	}
	if (_error) {
		_error->setGeometry(area);
	}
}

Window::SectionFactory PostFactory(
		const QString &postId,
		const QString &rootHint) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new PostSection(parent, controller, postId, rootHint));
	};
}

Window::SectionFactory ThreadFactory(const QString &postId) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new ThreadSection(parent, controller, postId));
	};
}

} // namespace Eli
