#include "stories/stories_boxes.h"

#include "boxes/peer_list_box.h"
#include "data/data_chats.h"
#include "data/data_feed.h"
#include "data/data_media.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "eli/eli_actions.h"
#include "history/history_view_media.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_video_player.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/image/image_view.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_stories.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtMultimedia/QMediaPlayer>

#include <optional>

namespace Stories {
namespace {

constexpr auto kMaxVideoMs = qint64(60'500); // media.max_video_duration_story
constexpr auto kShownVideoSeconds = 60;
constexpr auto kRecentLimit = 30;

enum class Audience {
	Everyone = 0,
	CloseFriends = 1,
};

// A fixed-size preview of the chosen file, centered in the box's row.
class Preview final : public Ui::RpWidget {
public:
	Preview(QWidget *parent, const QString &path, bool video)
	: RpWidget(parent) {
		const auto size = st::storiesComposePreview;
		if (video) {
			_video = Ui::CreateChild<Media::VideoPlayerWidget>(this);
			_video->setControlsShown(false);
			_video->setFullscreenAllowed(false);
			_video->setMuted(true);
			_video->setLooped(true);
			_video->setRadius(st::storiesComposePreviewRadius);
			_video->setSource(path);
			_video->resize(size);
			_video->play();
		} else {
			_image = Ui::CreateChild<Ui::ImageView>(this);
			_image->setRadius(st::storiesComposePreviewRadius);
			_image->setFit(true);
			_image->setSource(History::View::MakeLocalImageSource(path, 1280));
			_image->resize(size);
		}
		resize(size);
	}
	// A picture that is not a local file (a post's video poster).
	Preview(QWidget *parent, std::shared_ptr<Ui::ImageSource> source)
	: RpWidget(parent) {
		const auto size = st::storiesComposePreview;
		_image = Ui::CreateChild<Ui::ImageView>(this);
		_image->setRadius(st::storiesComposePreviewRadius);
		_image->setFit(true);
		_image->setSource(std::move(source));
		_image->resize(size);
		resize(size);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto size = st::storiesComposePreview;
		const auto child = _video
			? static_cast<QWidget*>(_video)
			: static_cast<QWidget*>(_image);
		if (child) {
			child->move((newWidth - size.width()) / 2, 0);
		}
		return size.height();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::storiesComposePreview;
		p.setPen(Qt::NoPen);
		p.setBrush(st::storiesComposePreviewBg);
		p.drawRoundedRect(
			QRect((width() - size.width()) / 2, 0, size.width(), size.height()),
			st::storiesComposePreviewRadius,
			st::storiesComposePreviewRadius);
	}

private:
	Media::VideoPlayerWidget *_video = nullptr;
	Ui::ImageView *_image = nullptr;

};

// Reads a local video's duration (Qt Multimedia, nothing is played).
void ProbeVideoDuration(
		QObject *guard,
		const QString &path,
		Fn<void(qint64 durationMs)> done) {
	const auto player = new QMediaPlayer(guard);
	const auto finish = [=](qint64 duration) {
		if (player->property("finished").toBool()) {
			return;
		}
		player->setProperty("finished", true);
		player->deleteLater();
		done(duration);
	};
	QObject::connect(player, &QMediaPlayer::durationChanged, player, [=](qint64 duration) {
		if (duration > 0) {
			finish(duration);
		}
	});
	QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player, [=](
			QMediaPlayer::MediaStatus status) {
		if (status == QMediaPlayer::LoadedMedia) {
			finish(player->duration());
		} else if (status == QMediaPlayer::InvalidMedia) {
			finish(0);
		}
	});
	QObject::connect(player, &QMediaPlayer::errorOccurred, player, [=] {
		finish(0);
	});
	player->setSource(QUrl::fromLocalFile(path));
}

void AddAudience(
		Ui::GenericBox *box,
		const std::shared_ptr<Ui::RadiobuttonGroup> &group) {
	box->addRow(Ui::object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_stories_audience(tr::now),
		st::boxLabel));
	box->addRow(Ui::object_ptr<Ui::Radiobutton>(
		box,
		group,
		int(Audience::Everyone),
		tr::lng_stories_audience_everyone(tr::now)));
	box->addRow(Ui::object_ptr<Ui::Radiobutton>(
		box,
		group,
		int(Audience::CloseFriends),
		tr::lng_stories_close_friends(tr::now)));
}

// Refuses a close-friends story while the list is known to be empty.
[[nodiscard]] bool CloseFriendsReady(
		Window::SessionController *controller,
		bool closeFriends) {
	const auto &settings = controller->session().data().stories().cachedSettings();
	if (closeFriends && settings && settings->closeFriendsCount <= 0) {
		controller->showToast(tr::lng_stories_close_friends_empty(tr::now));
		return false;
	}
	return true;
}

// ---- the viewers of my story

class ViewersController final : public Boxes::PeerListController {
public:
	explicit ViewersController(QString storyId)
	: _storyId(std::move(storyId)) {
	}

	QString title() const override {
		return tr::lng_stories_viewers(tr::now);
	}
	QString emptyText() const override {
		return _failed
			? tr::lng_error_load(tr::now)
			: tr::lng_stories_viewers_none(tr::now);
	}
	void prepare() override {
		delegate()->setLoading(true);
		load(QString());
	}
	void loadMore() override {
		if (!_loading && !_cursor.isEmpty()) {
			load(_cursor);
		}
	}
	void rowClicked(const Boxes::PeerListRow &row) override {
		Q_UNUSED(row);
	}

private:
	void load(const QString &cursor) {
		_loading = true;
		const auto first = cursor.isEmpty();
		session()->data().stories().viewers(
			_storyId,
			cursor,
			base::Guard(delegate()->peerListBox()),
			[=, this](const Data::StoryViewersPage &page) {
				_loading = false;
				_cursor = page.nextCursor;
				auto rows = QVector<Boxes::PeerListRow>();
				for (const auto &viewer : page.viewers) {
					auto row = Boxes::PeerListRow::FromUser(viewer.user);
					row.subtitle = Lang::RelativeTime(
						QDateTime::fromSecsSinceEpoch(viewer.viewedAt));
					if (!viewer.reaction.isEmpty()) {
						row.subtitle = viewer.reaction + u"  "_s + row.subtitle;
					}
					rows.push_back(row);
				}
				if (first) {
					delegate()->setRows(rows);
				} else {
					delegate()->appendRows(rows);
				}
			},
			[=, this](const Api::Error &) {
				_loading = false;
				if (first) {
					_failed = true; // not «no one has seen it»
				}
				delegate()->setLoading(false);
			});
	}

	QString _storyId;
	QString _cursor;
	bool _loading = false;
	bool _failed = false;

};

// ---- picking people

class PickController final : public Boxes::PeerListController {
public:
	PickController(
		QString title,
		QStringList preselected,
		Fn<void(const QStringList &ids, Ui::BoxContent *box)> done)
	: _title(std::move(title))
	, _preselected(std::move(preselected))
	, _done(std::move(done)) {
	}

	QString title() const override {
		return _title;
	}
	QString searchPlaceholder() const override {
		return tr::lng_stories_pick_search(tr::now);
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
	bool submitWithoutSelection() const override {
		return true;
	}
	QString submitText() const override {
		return tr::lng_stories_pick_save(tr::now);
	}

	void prepare() override {
		const auto session = this->session();
		auto &users = session->data().users();
		// Whoever is ticked already leads the list.
		for (const auto &id : _preselected) {
			auto user = users.get(id);
			if (!user.valid()) {
				user.id = id;
			}
			add(user);
		}
		auto count = 0;
		for (const auto &chat : session->data().chats().list(
				Data::Chats::Section::Messages)) {
			if (chat.saved
				|| chat.isGroup
				|| chat.isBot
				|| chat.anonymous
				|| chat.peerDeleted
				|| chat.peerUserId.isEmpty()) {
				continue;
			}
			auto user = users.get(chat.peerUserId);
			if (!user.valid()) {
				user.id = chat.peerUserId;
				user.displayName = chat.title;
			}
			add(user);
			if (++count >= kRecentLimit) {
				break;
			}
		}
		showBase();
		for (const auto &id : _preselected) {
			delegate()->setSelected(id, true);
		}
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().graph().following(
			session->userId(),
			QString(),
			guard,
			[this](const Data::GraphList &list) {
				for (const auto &profile : list.users) {
					if (!profile.deleted && !profile.me) {
						add(profile);
					}
				}
				if (delegate()->searchQuery().trimmed().isEmpty()) {
					showBase();
				}
			},
			[](const Api::Error &) {
			});
	}

	void searchQueryChanged(const QString &query) override {
		const auto request = ++_request;
		auto text = query.trimmed();
		while (text.startsWith(QLatin1Char('@'))) {
			text.remove(0, 1);
		}
		if (text.isEmpty()) {
			showBase();
			return;
		}
		delegate()->setLoading(true);
		const auto session = this->session();
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().graph().searchPeople(text, guard, [=, this](
				const QVector<Data::Profile> &people) {
			if (request != _request) {
				return;
			}
			auto rows = QVector<Boxes::PeerListRow>();
			for (const auto &profile : people) {
				if (!profile.deleted && profile.id != session->userId()) {
					rows.push_back(Boxes::PeerListRow::FromUser(profile));
					add(profile);
				}
			}
			setShown(rows);
		});
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		Q_UNUSED(row);
	}

	void submit(const QStringList &ids) override {
		if (_done) {
			_done(ids, delegate()->peerListBox());
		}
	}

private:
	void add(const Data::User &user) {
		if (!user.valid() || _known.contains(user.id)) {
			return;
		}
		_known.insert(user.id);
		_base.push_back(Boxes::PeerListRow::FromUser(user));
	}
	void showBase() {
		setShown(_base);
	}
	void setShown(const QVector<Boxes::PeerListRow> &rows) {
		const auto selected = delegate()->selectedIds();
		delegate()->setRows(rows);
		for (const auto &id : selected) {
			delegate()->setSelected(id, true);
		}
	}

	QString _title;
	QStringList _preselected;
	Fn<void(const QStringList &ids, Ui::BoxContent *box)> _done;
	QVector<Boxes::PeerListRow> _base;
	QSet<QString> _known;
	int _request = 0;

};

} // namespace

void ShowComposeBox(Window::SessionController *controller) {
	auto &stories = controller->session().data().stories();
	if (!stories.enabled()) {
		return;
	} else if (stories.posting().active) {
		controller->showToast(tr::lng_stories_busy(tr::now));
		return;
	}
	Support::PickFiles(
		controller->window(),
		{ .kind = Support::PickKind::Media, .multiple = false, .title = QString() },
		[=](const QStringList &paths) {
			if (!paths.isEmpty()) {
				ShowComposeBoxForFile(controller, paths.front());
			}
		});
}

void ShowComposeBoxForFile(
		Window::SessionController *controller,
		const QString &path) {
	const auto video = Support::IsVideoFile(path);
	if (!video && !Support::IsImageFile(path)) {
		controller->showToast(tr::lng_stories_file_unsupported(tr::now));
		return;
	}
	auto &stories = controller->session().data().stories();
	if (!stories.cachedSettings()) {
		// For the close-friends check of the audience.
		stories.settings(controller, nullptr);
	}
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_stories_new_title(tr::now));
		box->addRow(
			Ui::object_ptr<Preview>(box, path, video),
			st::boxRowPadding);
		const auto change = box->addRow(
			Ui::object_ptr<Ui::LinkButton>(box, tr::lng_stories_change_file(tr::now)),
			st::boxRowPadding,
			style::al_top);
		const auto caption = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::storiesComposeField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_stories_caption_hint(tr::now)));
		caption->setMaxLength(Data::Stories::kCaptionMax);
		const auto audience = std::make_shared<Ui::RadiobuttonGroup>(
			int(Audience::Everyone));
		AddAudience(box, audience);

		const auto durationOk = std::make_shared<bool>(!video);
		const auto send = std::make_shared<Fn<void()>>();
		const auto publish = box->addButton(tr::lng_stories_publish(tr::now), [=] {
			(*send)();
		}, st::defaultRoundButton);
		*send = [=] {
			if (!*durationOk) {
				return;
			}
			const auto closeFriends = (audience->value() == int(Audience::CloseFriends));
			if (!CloseFriendsReady(controller, closeFriends)) {
				return;
			}
			auto request = Data::PostStoryRequest();
			request.filePath = path;
			request.video = video;
			request.caption = caption->text().trimmed();
			request.closeFriends = closeFriends;
			if (!controller->session().data().stories().post(request)) {
				controller->showToast(tr::lng_stories_busy(tr::now));
				return;
			}
			box->closeBox();
		};
		// «الی»: the same file and words as a new post instead.
		box->addLeftButton(tr::lng_stories_to_eli(tr::now), [=] {
			auto args = Eli::ComposeArgs();
			args.prefill = caption->text().trimmed();
			args.files = QStringList{ path };
			box->closeBox();
			Eli::OpenCompose(controller, std::move(args));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		if (video) {
			publish->setLoading(true);
			const auto weak = QPointer<Ui::GenericBox>(box);
			ProbeVideoDuration(box, path, [=](qint64 duration) {
				if (!weak) {
					return;
				}
				publish->setLoading(false);
				if (duration > kMaxVideoMs) {
					controller->showToast(tr::lng_stories_video_too_long(
						tr::now,
						lt_seconds,
						Lang::Num(kShownVideoSeconds)));
					box->closeBox();
					return;
				}
				// Unknown (0): the server measures it while processing.
				*durationOk = true;
			});
		}
		change->setClickedCallback([=] {
			box->closeBox();
			ShowComposeBox(controller);
		});
		box->setFocusCallback([=] { caption->setFocusFast(); });
		QObject::connect(caption, &Ui::InputField::submitted, box, [=] {
			(*send)();
		});
	}));
}

// The video of a video post, when a story can hold it (Instagram's shared
// reel): the story plays it and carries the post's card.
[[nodiscard]] std::optional<Data::PostMedia> StoryVideoOf(
		Window::SessionController *controller,
		const QString &postId) {
	const auto post = controller->session().data().feed().post(postId);
	if (!post || post->media.isEmpty()) {
		return std::nullopt;
	}
	const auto &media = post->media.front();
	return (media.kind == u"video"
		&& media.durationMs > 0
		&& media.durationMs <= kMaxVideoMs)
		? std::make_optional(media)
		: std::nullopt;
}

void ShareToStory(Window::SessionController *controller, const QString &postId) {
	auto &stories = controller->session().data().stories();
	if (!stories.enabled() || postId.isEmpty()) {
		return;
	}
	if (!stories.cachedSettings()) {
		stories.settings(controller, nullptr);
	}
	const auto video = StoryVideoOf(controller, postId);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_stories_share_post_title(tr::now));
		if (video) {
			auto options = Support::ImageOptions();
			options.blurhash = video->blurhash;
			options.aspect = QSize(video->width, video->height);
			box->addRow(
				Ui::object_ptr<Preview>(box, Support::MakeImageSource(
					&controller->session(),
					Data::MediaRef::PostMedia(postId, video->id),
					Data::MediaVariant::Poster,
					options)),
				st::boxRowPadding);
			box->addRow(Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_stories_share_post_video(tr::now),
				st::boxLabel));
		}
		const auto caption = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::storiesComposeField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_stories_caption_hint(tr::now)));
		caption->setMaxLength(Data::Stories::kCaptionMax);
		const auto audience = std::make_shared<Ui::RadiobuttonGroup>(
			int(Audience::Everyone));
		AddAudience(box, audience);
		const auto busy = std::make_shared<bool>(false);
		const auto file = std::make_shared<Data::MediaHandlePtr>();
		const auto weak = QPointer<Ui::GenericBox>(box);
		const auto share = box->addButton(tr::lng_stories_share(tr::now), nullptr, st::defaultRoundButton);
		const auto unlock = [=] {
			*busy = false;
			*file = nullptr;
			if (weak) {
				share->setLoading(false);
			}
		};
		// The post's card on a gradient (post_id only): what every post can be.
		const auto createCard = [=](const QString &text, bool closeFriends) {
			auto request = Data::CreateStory();
			request.postId = postId;
			request.caption = text;
			request.closeFriends = closeFriends;
			controller->session().data().stories().create(request, controller, [=](
					const Data::Story &) {
				controller->showToast(tr::lng_stories_post_added(tr::now));
				if (weak) {
					weak->closeBox();
				}
			}, [=](const Api::Error &error) {
				unlock();
				if (error.code == u"story.post_unavailable") {
					controller->showToast(tr::lng_stories_post_cannot_share(tr::now));
				} else {
					Support::ShowError(controller, error);
				}
			});
		};
		// The video goes up as the story (media_id + post_id); without it the
		// card is posted instead.
		const auto postVideo = [=](const QString &text, bool closeFriends) {
			auto options = Data::Media::Options();
			options.decode = false;
			const auto handle = controller->session().data().media().request(
				Data::MediaRef::PostMedia(postId, video->id),
				Data::MediaVariant::Mp4,
				options);
			*file = handle;
			const auto fallback = [=] {
				if (*file == handle) {
					*file = nullptr;
					createCard(text, closeFriends);
				}
			};
			const auto ready = [=] {
				if (*file != handle) {
					return;
				}
				Support::SaveFileToTemp(
					handle->filePath(),
					handle->filePath().isEmpty() ? handle->bytes() : QByteArray(),
					u".mp4"_s,
					box,
					[=](const QString &path) {
						if (*file != handle) {
							return;
						} else if (path.isEmpty()) {
							fallback();
							return;
						}
						*file = nullptr;
						auto request = Data::PostStoryRequest();
						request.filePath = path;
						request.video = true;
						request.caption = text;
						request.closeFriends = closeFriends;
						request.postId = postId;
						if (!controller->session().data().stories().post(request)) {
							unlock();
							controller->showToast(tr::lng_stories_busy(tr::now));
							return;
						}
						box->closeBox();
					});
			};
			if (handle->isReady()) {
				ready();
			} else if (handle->isFailed()) {
				fallback();
			} else {
				QObject::connect(handle.get(), &Data::MediaHandle::ready, box, ready);
				QObject::connect(handle.get(), &Data::MediaHandle::failed, box, fallback);
			}
		};
		share->setClickedCallback([=] {
			if (*busy) {
				return;
			}
			const auto closeFriends = (audience->value() == int(Audience::CloseFriends));
			if (!CloseFriendsReady(controller, closeFriends)) {
				return;
			}
			const auto text = caption->text().trimmed();
			if (video && controller->session().data().stories().posting().active) {
				controller->showToast(tr::lng_stories_busy(tr::now));
				return;
			}
			*busy = true;
			share->setLoading(true);
			if (video) {
				postVideo(text, closeFriends);
			} else {
				createCard(text, closeFriends);
			}
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		box->setFocusCallback([=] { caption->setFocusFast(); });
	}));
}

void ShowViewersBox(Window::SessionController *controller, const QString &storyId) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<ViewersController>(storyId)));
}

void PickPeople(
		Window::SessionController *controller,
		const QString &title,
		const QStringList &preselected,
		Fn<void(const QStringList &ids)> done) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<PickController>(
			title,
			preselected,
			[=](const QStringList &ids, Ui::BoxContent *box) {
				if (done) {
					done(ids);
				}
				box->closeBox();
			})));
}

} // namespace Stories
