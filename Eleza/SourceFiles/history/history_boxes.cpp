#include "history/history_boxes.h"
#include "data/data_groups.h"
#include "data/data_group_rules.h"

#include "api/api_client_config.h"
#include "boxes/peer_list_box.h"
#include "core/core_application.h"
#include "core/core_settings.h"
#include "data/data_chats.h"
#include "data/data_image_decode.h"
#include "data/data_messages.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "history/history_album.h"
#include "history/history_compose_controls.h"
#include "history/history_compose_logic.h"
#include "history/history_logic.h"
#include "history/history_themes.h"
#include "history/history_view_media.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/file_dialogs.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtGui/QMouseEvent>

#include <algorithm>

namespace History {
namespace {

constexpr auto kMaxFilesInBox = 20;

// ---- the list of what is being sent

class FilesPreview final : public Ui::RpWidget {
public:
	explicit FilesPreview(QWidget *parent);

	void setFiles(const QStringList &paths);
	void setAsFiles(bool asFiles);
	void setSpoiler(bool spoiler);
	[[nodiscard]] QStringList paths() const;
	[[nodiscard]] bool hasVisual() const;
	[[nodiscard]] int count() const {
		return int(_items.size());
	}
	void setRemovedCallback(Fn<void()> callback) {
		_removed = std::move(callback);
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Item {
		QString path;
		QString name;
		qint64 size = 0;
		bool image = false;
		bool video = false;
		QSize dimensions;
		std::shared_ptr<Ui::ImageSource> source;
		QImage prepared;
		qint64 preparedKey = 0;
		QSize preparedSize;
	};

	[[nodiscard]] bool single() const;
	[[nodiscard]] QRect removeRect(int index) const; // logical
	[[nodiscard]] int rowAt(QPoint position) const;
	void paintThumb(Painter &p, Item &item, QRect box, int radius);

	std::vector<Item> _items;
	bool _asFiles = false;
	bool _spoiler = false;
	int _overRemove = -1;
	Fn<void()> _removed;

};

FilesPreview::FilesPreview(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
}

void FilesPreview::setFiles(const QStringList &paths) {
	auto items = std::vector<Item>();
	for (const auto &path : paths) {
		auto found = false;
		for (auto &item : _items) {
			if (item.path == path) {
				items.push_back(std::move(item));
				item.path = QString();
				found = true;
				break;
			}
		}
		if (found) {
			continue;
		}
		const auto info = QFileInfo(path);
		auto item = Item();
		item.path = path;
		item.name = info.fileName();
		item.size = info.size();
		item.image = Support::IsImageFile(path);
		item.video = Support::IsVideoFile(path);
		if (item.image) {
			item.dimensions = Data::ImageFileDimensions(path);
			item.source = View::MakeLocalImageSource(path);
			if (item.source) {
				connect(item.source.get(), &Ui::ImageSource::updated, this, [this] {
					update();
				});
			}
		}
		items.push_back(std::move(item));
	}
	_items = std::move(items);
	_overRemove = -1;
	resizeToWidth(width());
	update();
}

void FilesPreview::setAsFiles(bool asFiles) {
	if (_asFiles != asFiles) {
		_asFiles = asFiles;
		resizeToWidth(width());
		update();
	}
}

void FilesPreview::setSpoiler(bool spoiler) {
	if (_spoiler != spoiler) {
		_spoiler = spoiler;
		for (auto &item : _items) {
			item.prepared = QImage();
		}
		update();
	}
}

QStringList FilesPreview::paths() const {
	auto result = QStringList();
	for (const auto &item : _items) {
		result.push_back(item.path);
	}
	return result;
}

bool FilesPreview::hasVisual() const {
	return std::any_of(_items.begin(), _items.end(), [](const Item &item) {
		return item.image || item.video;
	});
}

bool FilesPreview::single() const {
	return (_items.size() == 1)
		&& _items.front().image
		&& !_asFiles
		&& _items.front().dimensions.isValid();
}

int FilesPreview::resizeGetHeight(int newWidth) {
	if (_items.empty()) {
		return 0;
	} else if (single()) {
		const auto size = _items.front().dimensions;
		const auto scaled = size.scaled(
			newWidth,
			st::historySendFilesPreviewHeight,
			Qt::KeepAspectRatio);
		return std::clamp(
			scaled.height(),
			int(st::historySendFilesRowHeight) * 2,
			int(st::historySendFilesPreviewHeight));
	}
	return int(_items.size()) * st::historySendFilesRowHeight;
}

QRect FilesPreview::removeRect(int index) const {
	const auto size = st::historySendFilesRemove.width;
	if (single()) {
		return QRect(width() - size - 6, 6, size, size);
	}
	const auto row = st::historySendFilesRowHeight;
	return QRect(width() - size, index * row + (row - size) / 2, size, size);
}

int FilesPreview::rowAt(QPoint position) const {
	for (auto i = 0; i != int(_items.size()); ++i) {
		if (myrtlrect(removeRect(i)).contains(position)) {
			return i;
		}
	}
	return -1;
}

void FilesPreview::paintThumb(Painter &p, Item &item, QRect box, int radius) {
	const auto ratio = devicePixelRatioF();
	auto image = item.source ? item.source->request(box.size() * ratio) : QImage();
	if (image.isNull() && item.source) {
		image = item.source->preview();
	}
	if (image.isNull()) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyMediaBg);
		p.drawRoundedRect(box, radius, radius);
		return;
	}
	if (item.prepared.isNull()
		|| item.preparedKey != image.cacheKey()
		|| item.preparedSize != box.size()) {
		auto cover = Ui::Images::PrepareCover(image, box.size(), ratio);
		if (_spoiler && !_asFiles) {
			cover = Ui::Images::Blur(std::move(cover), 24);
		}
		item.prepared = Ui::Images::Round(std::move(cover), radius);
		item.preparedKey = image.cacheKey();
		item.preparedSize = box.size();
	}
	p.drawImage(box.topLeft(), item.prepared);
}

void FilesPreview::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto paintRemove = [&](int index) {
		const auto box = myrtlrect(removeRect(index));
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(single()
				? st::historySendFilesRemoveBg
				: (_overRemove == index)
				? st::stateHover
				: st::transparent);
			p.drawEllipse(box);
		}
		const auto &icon = single()
			? st::historySendFilesRemove.icon
			: st::historySearchClose.icon;
		icon.paintInCenter(p, box);
	};
	if (single()) {
		auto &item = _items.front();
		const auto size = item.dimensions.scaled(
			width(),
			height(),
			Qt::KeepAspectRatio);
		const auto box = QRect(
			(width() - size.width()) / 2,
			(height() - size.height()) / 2,
			size.width(),
			size.height());
		paintThumb(p, item, box, st::historySendFilesRadius);
		if (_spoiler) {
			st::historyMediaSpoilerIcon.paintInCenter(p, box);
		}
		paintRemove(0);
		return;
	}
	const auto row = st::historySendFilesRowHeight;
	const auto thumb = row - 12;
	for (auto i = 0; i != int(_items.size()); ++i) {
		auto &item = _items[i];
		const auto top = i * row;
		const auto box = myrtlrect(0, top + (row - thumb) / 2, thumb, thumb);
		if (item.image && item.source) {
			paintThumb(p, item, box, st::historyFileIconRadius);
		} else {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyFileIconBg);
			p.drawRoundedRect(box, st::historyFileIconRadius, st::historyFileIconRadius);
			const auto type = FileTypeFor(item.name, QString());
			const auto &icon = item.video
				? st::historyFileVideoIcon
				: (type == FileType::Audio)
				? st::historyFileMusicIcon
				: st::historyFileIcon;
			icon.paintInCenter(p, box);
		}
		const auto left = thumb + st::historyFileSkip;
		const auto available = width() - left - st::historySendFilesRemove.width - 8;
		p.setPen(st::textPrimary);
		p.setFont(st::historySendFilesNameFont);
		p.drawTextLeft(
			left,
			top + row / 2 - st::historySendFilesNameFont->height,
			width(),
			st::historySendFilesNameFont->elided(item.name, available, Qt::ElideMiddle));
		p.setPen(st::textTertiary);
		p.setFont(st::historySendFilesSizeFont);
		p.drawTextLeft(
			left,
			top + row / 2 + 2,
			width(),
			Lang::Bytes(item.size));
		paintRemove(i);
	}
}

void FilesPreview::mouseMoveEvent(QMouseEvent *e) {
	const auto over = rowAt(e->pos());
	if (_overRemove != over) {
		_overRemove = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void FilesPreview::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_overRemove >= 0) {
		_overRemove = -1;
		setCursor(style::cur_default);
		update();
	}
}

void FilesPreview::mousePressEvent(QMouseEvent *e) {
	const auto index = rowAt(e->pos());
	if (e->button() != Qt::LeftButton || index < 0) {
		return;
	}
	_items.erase(_items.begin() + index);
	_overRemove = -1;
	resizeToWidth(width());
	update();
	if (_removed) {
		_removed();
	}
}

// ---- the box

class SendFilesBoxContent final : public Ui::BoxContent {
public:
	SendFilesBoxContent(
		QWidget *parent,
		Main::Session *session,
		const QStringList &paths,
		const QString &caption,
		Fn<void(const FilesToSend &)> send,
		QVector<int> selfDestructTimers);

	void setInnerFocus() override;

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void refresh();
	void layout();
	void addMore();
	void submit();
	[[nodiscard]] bool selfDestructOffered() const;
	[[nodiscard]] bool selfDestructOn() const;
	void showSelfDestructMenu();
	void setSelfDestruct(std::optional<Data::SelfDestructInput> value);

	Main::Session *_session = nullptr;
	QStringList _initial;
	QString _initialCaption;
	Fn<void(const FilesToSend &)> _send;
	Ui::ScrollArea *_scroll = nullptr;
	FilesPreview *_preview = nullptr;
	Ui::Checkbox *_asFiles = nullptr;
	Ui::Checkbox *_spoiler = nullptr;
	Ui::Checkbox *_grouped = nullptr;
	Ui::Checkbox *_captionAbove = nullptr;
	Ui::InputField *_caption = nullptr;
	QVector<int> _selfDestructTimers;
	std::optional<Data::SelfDestructInput> _selfDestruct;
	Ui::RoundButton *_selfDestructButton = nullptr;
	Ui::FlatLabel *_selfDestructHint = nullptr;

};

SendFilesBoxContent::SendFilesBoxContent(
	QWidget *parent,
	Main::Session *session,
	const QStringList &paths,
	const QString &caption,
	Fn<void(const FilesToSend &)> send,
	QVector<int> selfDestructTimers)
: BoxContent(parent)
, _session(session)
, _initial(paths.mid(0, kMaxFilesInBox))
, _initialCaption(caption)
, _send(std::move(send))
, _selfDestructTimers(std::move(selfDestructTimers)) {
}

void SendFilesBoxContent::prepare() {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this);
	_preview = _scroll->setOwnedWidget(Ui::object_ptr<FilesPreview>(_scroll));
	_asFiles = Ui::CreateChild<Ui::Checkbox>(
		this,
		tr::lng_history_send_as_files(tr::now));
	_spoiler = Ui::CreateChild<Ui::Checkbox>(
		this,
		tr::lng_history_send_spoiler(tr::now));
	_grouped = Ui::CreateChild<Ui::Checkbox>(
		this,
		tr::lng_history_send_grouped(tr::now),
		true);
	_captionAbove = Ui::CreateChild<Ui::Checkbox>(
		this,
		tr::lng_history_send_caption_above(tr::now));
	_selfDestructButton = Ui::CreateChild<Ui::RoundButton>(
		this,
		QString(),
		st::historySendFilesTimerButton);
	_selfDestructButton->setIcon(&st::historySendFilesTimerIcon);
	_selfDestructButton->setClickedCallback([this] { showSelfDestructMenu(); });
	_selfDestructHint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		QString(),
		st::historySendFilesTimerHint);
	_caption = Ui::CreateChild<Ui::InputField>(
		this,
		st::historySendFilesCaption,
		Ui::InputField::Mode::MultiLine,
		tr::lng_history_caption_placeholder(tr::now),
		_initialCaption);
	const auto config = _session->data().clientConfig();
	if (config && config->maxCaptionLength() > 0) {
		_caption->setMaxLength(config->maxCaptionLength());
	}
	_caption->setSubmitSettings(Core::App().settings().sendByCtrlEnter()
		? Ui::InputField::SubmitSettings::CtrlEnter
		: Ui::InputField::SubmitSettings::Enter);
	connect(_caption, &Ui::InputField::submitted, this, [this] { submit(); });
	connect(_caption, &Ui::RpWidget::heightChanged, this, [this] { layout(); });
	connect(_caption, &Ui::InputField::changed, this, [this] {
		// «Caption above» means something only with a caption.
		const auto wanted = _preview->hasVisual()
			&& !_asFiles->checked()
			&& !_caption->text().trimmed().isEmpty();
		if (wanted == _captionAbove->isHidden()) {
			refresh();
		}
	});
	connect(_asFiles, &Ui::Checkbox::checkedChanged, this, [this](bool checked) {
		_preview->setAsFiles(checked);
		refresh();
	});
	connect(_spoiler, &Ui::Checkbox::checkedChanged, this, [this](bool checked) {
		_preview->setSpoiler(checked);
	});
	_preview->setRemovedCallback([this] {
		if (!_preview->count()) {
			closeBox();
		} else {
			refresh();
		}
	});

	addButton(tr::lng_send(tr::now), [this] { submit(); }, st::defaultRoundButton);
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
	addLeftButton(tr::lng_history_add_files(tr::now), [this] { addMore(); });

	_preview->resizeToWidth(st::historySendFilesWidth
		- st::historySendFilesPadding.left()
		- st::historySendFilesPadding.right());
	_preview->setFiles(_initial);
	refresh();
}

void SendFilesBoxContent::refresh() {
	const auto count = _preview->count();
	const auto paths = _preview->paths();
	if (count == 1) {
		const auto &path = paths.front();
		setTitle(_asFiles->checked()
			? tr::lng_history_send_file_title(tr::now)
			: Support::IsImageFile(path)
			? tr::lng_history_send_photo_title(tr::now)
			: Support::IsVideoFile(path)
			? tr::lng_history_send_video_title(tr::now)
			: tr::lng_history_send_file_title(tr::now));
	} else {
		setTitle(tr::lng_history_send_files_title(tr::now, lt_count, count));
	}
	const auto visual = _preview->hasVisual();
	if (!selfDestructOffered()) {
		_selfDestruct = std::nullopt;
	}
	// Self-destructing: never as a file, with a spoiler or the caption above.
	const auto destructs = selfDestructOn();
	_asFiles->setVisible(visual && !destructs);
	_spoiler->setVisible(visual && !_asFiles->checked() && !destructs);
	_grouped->setVisible(count > 1);
	_captionAbove->setVisible(visual
		&& !destructs
		&& !_asFiles->checked()
		&& !_caption->text().trimmed().isEmpty());
	_selfDestructButton->setVisible(selfDestructOffered());
	_selfDestructButton->setText(!_selfDestruct
		? tr::lng_history_sd_option_off(tr::now)
		: _selfDestruct->viewOnce
		? tr::lng_history_sd_option_once(tr::now)
		: tr::lng_history_sd_option_timer(
			tr::now,
			lt_time,
			Support::SelfDestructTimerText(_selfDestruct->seconds)));
	_selfDestructHint->setText(!destructs
		? QString()
		: _selfDestruct->viewOnce
		? tr::lng_history_sd_hint_once(tr::now)
		: tr::lng_history_sd_hint_timer(
			tr::now,
			lt_time,
			Support::SelfDestructTimerText(_selfDestruct->seconds)));
	_selfDestructHint->setVisible(destructs);
	layout();
}

bool SendFilesBoxContent::selfDestructOffered() const {
	if (_selfDestructTimers.isEmpty() || _preview->count() != 1 || _asFiles->checked()) {
		return false;
	}
	const auto path = _preview->paths().front();
	return Support::IsImageFile(path) || Support::IsVideoFile(path);
}

bool SendFilesBoxContent::selfDestructOn() const {
	return _selfDestruct.has_value() && selfDestructOffered();
}

void SendFilesBoxContent::showSelfDestructMenu() {
	const auto menu = new Ui::PopupMenu(this);
	const auto add = [&](const QString &text, std::optional<Data::SelfDestructInput> value) {
		auto item = Ui::MenuItem();
		item.text = text;
		item.checked = (value.has_value() == _selfDestruct.has_value())
			&& (!value
				|| (value->viewOnce == _selfDestruct->viewOnce
					&& value->seconds == _selfDestruct->seconds));
		item.callback = [this, value] { setSelfDestruct(value); };
		menu->addItem(std::move(item));
	};
	add(tr::lng_history_sd_off(tr::now), std::nullopt);
	add(tr::lng_history_sd_view_once(tr::now), Data::SelfDestructInput{ .viewOnce = true });
	for (const auto seconds : std::as_const(_selfDestructTimers)) {
		add(
			Support::SelfDestructTimerText(seconds),
			Data::SelfDestructInput{ .seconds = seconds });
	}
	menu->popupAnchored(_selfDestructButton, style::al_left);
}

void SendFilesBoxContent::setSelfDestruct(std::optional<Data::SelfDestructInput> value) {
	_selfDestruct = value;
	if (selfDestructOn()) {
		_spoiler->setChecked(false, false);
		_captionAbove->setChecked(false, false);
	}
	refresh();
}

void SendFilesBoxContent::layout() {
	const auto &padding = st::historySendFilesPadding;
	const auto width = st::historySendFilesWidth;
	const auto inner = width - padding.left() - padding.right();
	_preview->resizeToWidth(inner);
	const auto listHeight = std::min(
		_preview->height(),
		int(st::historySendFilesPreviewHeight));
	auto top = padding.top();
	_scroll->setGeometry(padding.left(), top, inner, listHeight);
	top += listHeight + st::historySendFilesSkip;
	const auto &check = st::historySendFilesCheckPadding;
	for (const auto box : { _asFiles, _spoiler, _grouped, _captionAbove }) {
		if (box->isHidden()) {
			continue;
		}
		box->resizeToWidth(width - check.left() - check.right());
		box->moveToLeft(check.left(), top + check.top(), width);
		top += check.top() + box->height() + check.bottom();
	}
	if (!_selfDestructButton->isHidden()) {
		_selfDestructButton->resizeToNaturalWidth(inner);
		_selfDestructButton->moveToLeft(check.left(), top + check.top(), width);
		top += check.top() + _selfDestructButton->height() + check.bottom();
	}
	if (!_selfDestructHint->isHidden()) {
		_selfDestructHint->resizeToWidth(width - 2 * check.left());
		_selfDestructHint->moveToLeft(check.left() + check.left(), top, width);
		top += _selfDestructHint->height() + check.bottom();
	}
	top += st::historySendFilesSkip;
	_caption->resizeToWidth(inner);
	_caption->moveToLeft(padding.left(), top, width);
	top += _caption->height() + padding.bottom();
	setDimensions(width, top);
}

void SendFilesBoxContent::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
}

void SendFilesBoxContent::setInnerFocus() {
	_caption->setFocusFast();
}

void SendFilesBoxContent::addMore() {
	auto args = Support::PickArgs();
	args.kind = Support::PickKind::Any;
	args.multiple = true;
	Support::PickFiles(window(), args, base::guard(this, [this](const QStringList &paths) {
		auto all = _preview->paths();
		for (const auto &path : paths) {
			if (!all.contains(path) && all.size() < kMaxFilesInBox) {
				all.push_back(path);
			}
		}
		_preview->setFiles(all);
		refresh();
	}));
}

void SendFilesBoxContent::submit() {
	auto result = FilesToSend();
	result.paths = _preview->paths();
	if (result.paths.isEmpty()) {
		closeBox();
		return;
	}
	const auto prepared = PrepareForSend(_caption->text(), {});
	result.caption = prepared.text;
	result.entities = prepared.entities;
	result.asFiles = _asFiles->checked() || !_preview->hasVisual();
	result.spoiler = !result.asFiles && _spoiler->checked();
	result.grouped = _grouped->checked();
	result.captionAbove = !result.asFiles
		&& !result.caption.isEmpty()
		&& _captionAbove->checked();
	if (selfDestructOn()) {
		result.selfDestruct = _selfDestruct;
		result.spoiler = false;
		result.captionAbove = false;
		result.asFiles = false;
	}
	if (const auto send = std::exchange(_send, nullptr)) {
		send(result);
	}
	closeBox();
}

// ---- chat themes

class ThemesGrid final : public Ui::RpWidget {
public:
	ThemesGrid(QWidget *parent, const QString &current, Fn<void(const QString &)> chosen);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] int columns() const;
	[[nodiscard]] QRect cellRect(int index) const; // logical
	[[nodiscard]] int cellAt(QPoint position) const;

	QString _current;
	Fn<void(const QString &)> _chosen;
	int _over = -1;

};

ThemesGrid::ThemesGrid(
	QWidget *parent,
	const QString &current,
	Fn<void(const QString &)> chosen)
: RpWidget(parent)
, _current(IsKnownChatTheme(current) ? current : QString())
, _chosen(std::move(chosen)) {
	setMouseTracking(true);
}

int ThemesGrid::columns() const {
	const auto cell = st::historyThemeCell.width() + st::historyThemeSkip;
	return std::max((width() + st::historyThemeSkip) / cell, 1);
}

QRect ThemesGrid::cellRect(int index) const {
	const auto count = columns();
	const auto cell = st::historyThemeCell;
	const auto used = count * cell.width() + (count - 1) * st::historyThemeSkip;
	const auto left = (width() - used) / 2;
	return QRect(
		left + (index % count) * (cell.width() + st::historyThemeSkip),
		(index / count) * (cell.height() + st::historyThemeSkip),
		cell.width(),
		cell.height());
}

int ThemesGrid::cellAt(QPoint position) const {
	const auto count = int(ChatThemePresets().size());
	for (auto i = 0; i != count; ++i) {
		if (myrtlrect(cellRect(i)).contains(position)) {
			return i;
		}
	}
	return -1;
}

int ThemesGrid::resizeGetHeight(int newWidth) {
	const auto cell = st::historyThemeCell.width() + st::historyThemeSkip;
	const auto perRow = std::max((newWidth + st::historyThemeSkip) / cell, 1);
	const auto count = int(ChatThemePresets().size());
	const auto rows = (count + perRow - 1) / perRow;
	return rows * st::historyThemeCell.height() + (rows - 1) * st::historyThemeSkip;
}

void ThemesGrid::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto &presets = ChatThemePresets();
	const auto nameHeight = st::historyThemeNameFont->height + 8;
	for (auto i = 0; i != int(presets.size()); ++i) {
		const auto &preset = presets[i];
		const auto cell = myrtlrect(cellRect(i));
		const auto card = cell.adjusted(0, 0, 0, -nameHeight);
		const auto colors = ColorsForTheme(preset.id, false);
		auto path = QPainterPath();
		path.addRoundedRect(card, st::historyThemeRadius, st::historyThemeRadius);
		p.save();
		p.setClipPath(path);
		PaintWallpaper(p, card, card.topLeft(), preset.id);
		const auto radius = st::historyThemeBubbleRadius;
		const auto bubbleHeight = 18;
		p.setPen(Qt::NoPen);
		p.setBrush(colors.bubbleIn);
		p.drawRoundedRect(
			QRect(card.x() + 8, card.y() + 14, card.width() * 55 / 100, bubbleHeight),
			radius,
			radius);
		p.setBrush(colors.bubbleOut);
		const auto outWidth = card.width() * 62 / 100;
		p.drawRoundedRect(
			QRect(card.right() + 1 - 8 - outWidth, card.y() + 14 + bubbleHeight + 8, outWidth, bubbleHeight),
			radius,
			radius);
		p.drawRoundedRect(
			QRect(card.right() + 1 - 8 - outWidth * 2 / 3, card.y() + 14 + 2 * (bubbleHeight + 8) - 4, outWidth * 2 / 3, bubbleHeight),
			radius,
			radius);
		p.restore();
		const auto active = (preset.id == _current);
		if (active || _over == i) {
			auto pen = QPen(active ? st::historyThemeActiveBorder->c : st::borderStrong->c);
			pen.setWidthF(active ? 2.5 : 1.5);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(card).adjusted(1, 1, -1, -1),
				st::historyThemeRadius,
				st::historyThemeRadius);
		}
		p.setPen(active ? st::brandText : st::textSecondary);
		p.setFont(st::historyThemeNameFont);
		p.drawText(
			QRect(cell.x(), card.bottom() + 5, cell.width(), nameHeight - 4),
			Qt::AlignHCenter | Qt::AlignTop,
			st::historyThemeNameFont->elided(ChatThemeName(preset.id), cell.width()));
	}
}

void ThemesGrid::mouseMoveEvent(QMouseEvent *e) {
	const auto over = cellAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void ThemesGrid::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

void ThemesGrid::mousePressEvent(QMouseEvent *e) {
	const auto index = cellAt(e->pos());
	if (e->button() != Qt::LeftButton || index < 0) {
		return;
	}
	_current = ChatThemePresets()[index].id;
	update();
	if (_chosen) {
		_chosen(_current);
	}
}

[[nodiscard]] QString PeerName(Main::Session *session, const Data::Chat &chat) {
	return Support::ChatTitle(session, chat);
}

} // namespace

void ForwardMessages(
		Main::Session *session,
		const QString &chat,
		const QVector<Data::Message> &messages) {
	const auto config = session->data().clientConfig();
	const auto groups = ForwardAlbumGroups(
		messages,
		config ? config->maxAlbumItems() : 10);
	auto ids = QHash<int, QString>();
	auto &outbox = session->data().outbox();
	for (auto i = 0; i != int(messages.size()); ++i) {
		auto album = QString();
		if (groups[i] >= 0) {
			auto &id = ids[groups[i]];
			if (id.isEmpty()) {
				id = Data::Outbox::NewAlbumId();
			}
			album = id;
		}
		outbox.forward(chat, messages[i], album);
	}
}

Ui::object_ptr<Ui::BoxContent> SendFilesBox(
		Main::Session *session,
		const QStringList &paths,
		const QString &caption,
		Fn<void(const FilesToSend &)> send,
		QVector<int> selfDestructTimers) {
	return Ui::Box<SendFilesBoxContent>(
		session,
		paths,
		caption,
		std::move(send),
		std::move(selfDestructTimers));
}

std::optional<Data::Location> ParseLocation(const QString &text) {
	static const auto kNumber = u"(-?\\d{1,3}(?:\\.\\d+)?)"_s;
	static const auto kPatterns = std::vector<QRegularExpression>{
		QRegularExpression(u"@"_s + kNumber + u","_s + kNumber),
		QRegularExpression(u"[?&](?:q|ll|query|center|destination)="_s
			+ kNumber
			+ u"(?:,|%2C)"_s
			+ kNumber,
			QRegularExpression::CaseInsensitiveOption),
		QRegularExpression(u"mlat="_s + kNumber + u".*mlon="_s + kNumber),
		QRegularExpression(u"#map=\\d+/"_s + kNumber + u"/"_s + kNumber),
		QRegularExpression(u"^\\s*(?:geo:)?\\s*"_s
			+ kNumber
			+ u"\\s*[,;\\s]\\s*"_s
			+ kNumber),
	};
	const auto latin = Lang::Latinize(text)
		.replace(QChar(0x066B), u'.')
		.replace(QChar(0x060C), u',');
	for (const auto &pattern : kPatterns) {
		const auto match = pattern.match(latin);
		if (!match.hasMatch()) {
			continue;
		}
		auto okLat = false;
		auto okLng = false;
		const auto lat = match.captured(1).toDouble(&okLat);
		const auto lng = match.captured(2).toDouble(&okLng);
		if (okLat && okLng && std::abs(lat) <= 90. && std::abs(lng) <= 180.) {
			auto result = Data::Location();
			result.lat = lat;
			result.lng = lng;
			return result;
		}
	}
	return std::nullopt;
}

Ui::object_ptr<Ui::BoxContent> LocationBox(Fn<void(const Data::Location &)> send) {
	return Ui::Box([send = std::move(send)](Ui::GenericBox *box) {
		box->setTitle(tr::lng_history_location_title(tr::now));
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_history_location_about(tr::now),
			st::historyBoxText));
		const auto coords = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::historyLocationField,
			tr::lng_history_location_coords(tr::now)));
		coords->setForcedDirection(Qt::LeftToRight);
		const auto label = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::historyLocationField,
			tr::lng_history_location_label(tr::now)));
		label->setMaxLength(64);
		box->setFocusCallback([=] { coords->setFocusFast(); });
		const auto submit = [=] {
			auto location = ParseLocation(coords->text());
			if (!location) {
				coords->showError();
				return;
			}
			location->label = label->text().trimmed();
			box->closeBox();
			if (send) {
				send(*location);
			}
		};
		QObject::connect(coords, &Ui::InputField::submitted, box, submit);
		QObject::connect(label, &Ui::InputField::submitted, box, submit);
		box->addButton(tr::lng_send(tr::now), submit, st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	});
}

Ui::object_ptr<Ui::BoxContent> ChatThemeBox(
		Main::Session *session,
		const QString &chatId) {
	return Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_history_theme_title(tr::now));
		box->setWidth(st::historySendFilesWidth);
		const auto chat = session->data().chats().chat(chatId);
		const auto current = chat ? chat->theme : QString();
		box->addRow(
			Ui::object_ptr<ThemesGrid>(box, current, [=](const QString &id) {
				session->data().chats().setTheme(chatId, id);
				box->closeBox();
			}),
			st::historyThemeBoxPadding);
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	});
}

void ShowDeleteMessagesBox(
		Window::SessionController *controller,
		const QString &chatId,
		const QVector<Data::Message> &messages,
		Fn<void()> done) {
	if (messages.isEmpty()) {
		return;
	}
	const auto session = &controller->session();
	const auto chat = session->data().chats().chat(chatId);
	const auto config = session->data().clientConfig();
	const auto window = config ? config->deleteForEveryoneWindow() : qint64(0);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto group = chat && chat->isGroup;
	const auto direct = chat && !chat->isConversation && !chat->saved && !group;
	const auto conversation = chat && chat->isConversation;
	// A group: my own messages (inside the server's window, when it sets
	// one) and, with the right to delete messages, anybody's.
	const auto groupWindow = group
		? session->data().groups().deleteOwnWindow() * 1000
		: qint64(0);
	const auto deleteRight = group
		&& chat->group
		&& chat->group->hasRight(u"delete_messages");
	// Whether the other side can lose this message too.
	const auto revocable = [=](const Data::Message &message) {
		if (group) {
			return Data::GroupRules::CanDeleteForEveryone(
				message.mine,
				message.kind == Data::MessageKind::System,
				message.seq,
				message.createdAt,
				now,
				groupWindow,
				deleteRight);
		}
		if (message.seq <= 0 || !chat || chat->saved || chat->peerDeleted) {
			return false;
		} else if (conversation && !message.mine) {
			return false;
		} else if (!direct && !conversation) {
			return false;
		}
		return (window <= 0) || (now - message.createdAt <= window * 1000);
	};
	const auto anyRevocable = std::any_of(messages.begin(), messages.end(), revocable);
	const auto forEveryone = std::make_shared<bool>(false);

	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_chat_delete_message(tr::now);
	args.text = (messages.size() == 1)
		? tr::lng_history_delete_sure(tr::now)
		: tr::lng_history_delete_sure_many(tr::now, lt_count, int(messages.size()));
	args.confirmText = tr::lng_delete(tr::now);
	args.destructive = true;
	if (anyRevocable) {
		args.checkboxText = group
			? tr::lng_groups_delete_also_everyone(tr::now)
			: tr::lng_history_delete_also_for(
				tr::now,
				lt_name,
				PeerName(session, *chat));
		args.checkboxChecked = false;
		args.checkboxChanged = [=](bool checked) {
			*forEveryone = checked;
		};
	}
	args.confirmed = [=](std::function<void()> close) {
		auto &data = session->data();
		auto everyone = QVector<qint64>();
		for (const auto &message : messages) {
			if (message.local()) {
				data.outbox().discard(message.clientMessageId);
			} else if (group && *forEveryone && revocable(message)) {
				// One change for all of them.
				everyone.push_back(message.seq);
			} else {
				data.messages().deleteMessage(
					chatId,
					message.seq,
					*forEveryone && revocable(message));
			}
		}
		if (everyone.size() > 1 && data.groups().administration()) {
			data.groups().deleteMessages(chatId, everyone);
		} else {
			// (The bulk call came with the second release of groups.)
			for (const auto seq : std::as_const(everyone)) {
				data.messages().deleteMessage(chatId, seq, true);
			}
		}
		close();
		if (done) {
			done();
		}
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowForwardBox(
		Window::SessionController *controller,
		const QVector<Data::Message> &messages,
		Fn<void()> done) {
	auto sorted = QVector<Data::Message>();
	for (const auto &message : messages) {
		if (CanForward(message)) {
			sorted.push_back(message);
		}
	}
	if (sorted.isEmpty()) {
		return;
	}
	std::stable_sort(sorted.begin(), sorted.end(), Data::MessageLess);
	const auto session = &controller->session();
	auto args = Boxes::ChooseChatsArgs();
	args.title = tr::lng_history_forward_title(tr::now);
	args.submitText = tr::lng_forward(tr::now);
	args.multiSelect = true;
	args.includeSaved = true;
	args.includeConversations = false;
	args.onlyWritable = true;
	const auto guard = QPointer<Window::SessionController>(controller);
	controller->show(Boxes::ChooseChatsBox(session, std::move(args), [=](
			const QStringList &chatIds) {
		if (chatIds.isEmpty() || !guard) {
			return;
		}
		if (chatIds.size() == 1) {
			// The chat opens with the messages attached, a
			// comment can be typed before they go out.
			SetPendingForward(session, chatIds.front(), sorted);
			guard->openRoute(Window::Routes::Chat(chatIds.front()));
		} else {
			for (const auto &chat : chatIds) {
				ForwardMessages(session, chat, sorted);
			}
			guard->showToast(tr::lng_history_forwarded_toast(
				tr::now,
				lt_count,
				int(chatIds.size())));
		}
		if (done) {
			done();
		}
	}));
}

void ShowClearHistoryBox(
		Window::SessionController *controller,
		const QString &chatId) {
	const auto session = &controller->session();
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_history_clear_history(tr::now);
	args.text = tr::lng_history_clear_sure(tr::now);
	args.confirmText = tr::lng_history_clear_confirm(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		session->data().chats().deleteChat(chatId, false);
		close();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowDeleteChatBox(
		Window::SessionController *controller,
		const QString &chatId) {
	const auto session = &controller->session();
	const auto chat = session->data().chats().chat(chatId);
	if (!chat) {
		return;
	}
	const auto config = session->data().clientConfig();
	const auto name = PeerName(session, *chat);
	const auto canBoth = !chat->saved
		&& !chat->isConversation
		&& !chat->peerDeleted
		&& (!config || config->deleteChatForBothEnabled());
	const auto forBoth = std::make_shared<bool>(false);
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_chat_delete_chat(tr::now);
	args.text = chat->saved
		? tr::lng_history_delete_saved_sure(tr::now)
		: tr::lng_history_delete_chat_sure(tr::now, lt_name, name);
	args.confirmText = tr::lng_delete(tr::now);
	args.destructive = true;
	if (canBoth) {
		args.checkboxText = tr::lng_history_delete_also_for(tr::now, lt_name, name);
		args.checkboxChecked = false;
		args.checkboxChanged = [=](bool checked) {
			*forBoth = checked;
		};
	}
	const auto guard = QPointer<Window::SessionController>(controller);
	args.confirmed = [=](std::function<void()> close) {
		session->data().chats().deleteChat(chatId, *forBoth);
		close();
		if (guard) {
			guard->clearMainStack(Window::Tab::Chats);
		}
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

} // namespace History
