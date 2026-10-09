#include "support/media_image.h"

#include "data/data_blurhash.h"
#include "data/data_chats.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "styles/style_widgets.h"

namespace Support {
namespace {

constexpr auto kFitStep = 160;
constexpr auto kMaxUserpicEntries = 600;
constexpr auto kLargeUserpic = 120;

[[nodiscard]] QSize RoundedFit(QSize size) {
	const auto round = [](int value) {
		return ((std::max(value, 1) + kFitStep - 1) / kFitStep) * kFitStep;
	};
	return QSize(round(size.width()), round(size.height()));
}

[[nodiscard]] QImage BlurhashPreview(const QString &hash, QSize aspect) {
	if (hash.isEmpty()) {
		return QImage();
	}
	auto width = 32;
	auto height = 32;
	if (aspect.width() > 0 && aspect.height() > 0) {
		if (aspect.width() >= aspect.height()) {
			height = std::max(32 * aspect.height() / aspect.width(), 4);
		} else {
			width = std::max(32 * aspect.width() / aspect.height(), 4);
		}
	}
	return Data::BlurhashImage(hash, width, height);
}

class MediaImageSource final : public Ui::ImageSource {
public:
	MediaImageSource(
		Main::Session *session,
		const Data::MediaRef &ref,
		Data::MediaVariant variant,
		ImageOptions options)
	: _session(session)
	, _ref(ref)
	, _variant(variant)
	, _options(std::move(options)) {
		if (_options.preview.isNull()) {
			_options.preview = BlurhashPreview(
				_options.blurhash,
				_options.aspect);
		}
	}

	QImage request(QSize size) override {
		if (_handle && _handle->isReady()) {
			// A much larger target than what was decoded: decode again.
			const auto &image = _handle->image();
			if (_options.original
				|| !_options.fit.isEmpty()
				|| (size.width() <= _fit.width() && size.height() <= _fit.height())
				|| image.isNull()) {
				return image;
			}
		} else if (_handle) {
			// Loading, or failed for good (no retry storm from repaints).
			return QImage();
		}
		if (!_session || !_ref.valid()) {
			_failed = true;
			return QImage();
		}
		const auto previous = (_handle && _handle->isReady())
			? _handle->image()
			: QImage();
		auto options = Data::Media::Options();
		if (!_options.original) {
			_fit = _options.fit.isEmpty() ? RoundedFit(size) : _options.fit;
			options.fit = _fit;
		}
		_failed = false;
		_handle = _session->data().media().request(_ref, _variant, options);
		if (_handle->isReady()) {
			return _handle->image();
		}
		QObject::connect(_handle.get(), &Data::MediaHandle::ready, this, [this] {
			emit updated();
		});
		QObject::connect(_handle.get(), &Data::MediaHandle::failed, this, [this] {
			_failed = true;
			emit updated();
		});
		// Keep showing the smaller image while the larger one loads.
		return previous;
	}

	QImage preview() const override {
		return _options.preview;
	}

	bool failed() const override {
		return _failed;
	}

private:
	QPointer<Main::Session> _session;
	Data::MediaRef _ref;
	Data::MediaVariant _variant = Data::MediaVariant::Thumb;
	ImageOptions _options;
	Data::MediaHandlePtr _handle;
	QSize _fit;
	bool _failed = false;

};

} // namespace

std::shared_ptr<Ui::ImageSource> MakeImageSource(
		Main::Session *session,
		const Data::MediaRef &ref,
		Data::MediaVariant variant,
		ImageOptions options) {
	if (!session || !ref.valid()) {
		return nullptr;
	}
	return std::make_shared<MediaImageSource>(
		session,
		ref,
		variant,
		std::move(options));
}

std::shared_ptr<Ui::ImageSource> MakeMessageImageSource(
		Main::Session *session,
		const Data::Message &message,
		Data::MediaVariant variant) {
	switch (message.kind) {
	case Data::MessageKind::Photo:
	case Data::MessageKind::Gif:
	case Data::MessageKind::Sticker:
		break;
	case Data::MessageKind::Video:
	case Data::MessageKind::VideoNote:
		if (variant != Data::MediaVariant::Thumb) {
			variant = Data::MediaVariant::Poster;
		}
		break;
	default:
		return nullptr;
	}
	auto options = ImageOptions();
	if (message.media) {
		options.blurhash = message.media->blurhash;
		options.aspect = QSize(message.media->width, message.media->height);
	}
	return MakeImageSource(
		session,
		Data::MediaRef::Attachment(message.chat, message.seq),
		variant,
		std::move(options));
}

std::shared_ptr<Ui::ImageSource> UserpicSource(
		Main::Session *session,
		const Data::User &user,
		Data::MediaVariant variant) {
	if (!session || user.avatarId.isEmpty() || user.deleted) {
		return nullptr;
	}
	auto options = ImageOptions();
	// One decoded copy serves every size of the same picture.
	options.original = true;
	return MakeImageSource(
		session,
		Data::MediaRef::Avatar(user.id, user.avatarId),
		variant,
		std::move(options));
}

Ui::UserpicKind UserpicKindFor(const Data::User &user) {
	return user.deleted ? Ui::UserpicKind::Deleted : Ui::UserpicKind::User;
}

UserpicButton::UserpicButton(
	QWidget *parent,
	Main::Session *session,
	const QString &userId,
	int size)
: UserpicWidget(parent, size)
, _session(session)
, _userId(userId)
, _size(size) {
	if (session) {
		auto &data = session->data();
		QObject::connect(
			&data.users(),
			&Data::Users::changed,
			this,
			[this](const QStringList &ids) {
				if (!_special && ids.contains(_userId)) {
					refresh();
				}
			});
		QObject::connect(
			&data.users(),
			&Data::Users::meChanged,
			this,
			[this] {
				if (!_special
					&& _session
					&& _userId == _session->data().users().myId()) {
					refresh();
				}
			});
		QObject::connect(
			&data.chats(),
			&Data::Chats::presenceChanged,
			this,
			[this](const QString &user) {
				if (user == _userId) {
					refreshOnline();
				}
			});
	}
	refresh();
}

UserpicButton::~UserpicButton() {
	watch(false);
}

void UserpicButton::setUserId(const QString &userId) {
	if (_userId == userId && !_special) {
		return;
	}
	watch(false);
	_special = false;
	_userId = userId;
	_avatarId = QString();
	setSource(nullptr);
	refresh();
	watch(_showOnline);
	refreshOnline();
}

void UserpicButton::setSpecial(
		Ui::UserpicKind kind,
		const QString &name,
		const QString &id) {
	watch(false);
	_special = true;
	_userId = QString();
	_avatarId = QString();
	setSource(nullptr);
	setOnline(false);
	setPlaceholder(id, name, kind);
}

void UserpicButton::setShowOnline(bool show) {
	if (_showOnline == show) {
		return;
	}
	_showOnline = show;
	watch(show && !_special);
	refreshOnline();
}

void UserpicButton::watch(bool watching) {
	if (_watching == watching || !_session || _userId.isEmpty()) {
		return;
	}
	_watching = watching;
	auto &chats = _session->data().chats();
	if (watching) {
		chats.watchPresence({ _userId });
	} else {
		chats.unwatchPresence({ _userId });
	}
}

void UserpicButton::refreshOnline() {
	if (!_session || !_showOnline || _special) {
		setOnline(false);
		return;
	}
	const auto presence = _session->data().chats().presence(_userId);
	setOnline(presence && presence->online());
}

void UserpicButton::refresh() {
	if (!_session || _special) {
		return;
	}
	auto &users = _session->data().users();
	const auto me = (_userId == users.myId()) && users.me().valid();
	const auto user = me ? users.me().card() : users.get(_userId);
	setPlaceholder(_userId, user.displayName, UserpicKindFor(user));
	const auto avatarId = user.deleted ? QString() : user.avatarId;
	if (avatarId != _avatarId || avatarId.isEmpty()) {
		_avatarId = avatarId;
		setSource(UserpicSource(
			_session,
			user,
			(_size > kLargeUserpic)
				? Data::MediaVariant::Feed
				: Data::MediaVariant::Thumb));
	}
}

UserpicPainter::UserpicPainter(
	Main::Session *session,
	std::function<void(const QString &userId)> update,
	QObject *parent)
: QObject(parent)
, _session(session)
, _update(std::move(update)) {
	style::OnPaletteChanged(this, [this] { clear(); });
}

void UserpicPainter::clear() {
	_entries.clear();
}

void UserpicPainter::paintSpecial(
		QPainter &p,
		const QRect &rect,
		Ui::UserpicKind kind,
		const QString &id,
		const QString &name) {
	Ui::PaintUserpicPlaceholder(p, rect, id, name, kind);
}

void UserpicPainter::paint(
		QPainter &p,
		const QRect &rect,
		const QString &userId) {
	if (!_session || userId.isEmpty()) {
		Ui::PaintUserpicPlaceholder(
			p,
			rect,
			userId,
			QString(),
			Ui::UserpicKind::Deleted);
		return;
	}
	auto &users = _session->data().users();
	const auto me = (userId == users.myId()) && users.me().valid();
	auto user = me ? users.me().card() : users.get(userId);
	if (!user.valid()) {
		// Unknown yet: Data::Users fetches it and the list repaints on changed().
		user.id = userId;
	}
	paint(p, rect, user);
}

void UserpicPainter::paint(
		QPainter &p,
		const QRect &rect,
		const Data::User &user) {
	const auto placeholder = [&] {
		Ui::PaintUserpicPlaceholder(
			p,
			rect,
			user.id,
			user.displayName,
			UserpicKindFor(user));
	};
	if (!_session || user.avatarId.isEmpty() || user.deleted) {
		placeholder();
		return;
	}
	const auto size = rect.width();
	const auto ratio = p.device()->devicePixelRatioF();
	auto i = _entries.find(user.id);
	if (i == _entries.end() || i->avatarId != user.avatarId) {
		if (_entries.size() >= kMaxUserpicEntries) {
			_entries.clear();
		}
		i = _entries.insert(user.id, Entry());
		i->avatarId = user.avatarId;
	}
	auto &entry = *i;
	if (!entry.prepared.isNull()
		&& entry.size == size
		&& entry.ratio == ratio) {
		p.drawImage(rect, entry.prepared);
		return;
	}
	const auto ref = Data::MediaRef::Avatar(user.id, user.avatarId);
	const auto variant = (size > kLargeUserpic)
		? Data::MediaVariant::Feed
		: Data::MediaVariant::Thumb;
	auto &media = _session->data().media();
	auto image = (entry.handle && entry.handle->isReady())
		? entry.handle->image()
		: media.cached(ref, variant);
	if (!image.isNull()) {
		entry.prepared = Ui::PrepareUserpic(image, size, ratio);
		entry.size = size;
		entry.ratio = ratio;
		p.drawImage(rect, entry.prepared);
		return;
	}
	placeholder();
	if (entry.handle) {
		return;
	}
	entry.handle = media.request(ref, variant);
	const auto userId = user.id;
	const auto avatarId = user.avatarId;
	const auto raw = entry.handle.get();
	QObject::connect(raw, &Data::MediaHandle::ready, this, [=, this] {
		const auto found = _entries.find(userId);
		if (found != _entries.end() && found->avatarId == avatarId) {
			found->prepared = QImage();
		}
		if (_update) {
			_update(userId);
		}
	});
	QObject::connect(raw, &Data::MediaHandle::failed, this, [=, this] {
		const auto found = _entries.find(userId);
		if (found != _entries.end() && found->avatarId == avatarId) {
			found->failed = true;
		}
	});
}

} // namespace Support
