// «به‌روزرسانی»: the installed version, what the checker knows (with the
// action that fits: check, download, restart to update, open the download
// page), «به‌روزرسانی خودکار» and the notes of the new / the running version.
#include "settings/settings_pages.h"

#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "settings/settings_common.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/widgets/labels.h"
#include "updater/updater_checker.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_settings_update.h"

namespace Settings {
namespace {

[[nodiscard]] QString Percent(qint64 received, qint64 total) {
	const auto value = (total > 0)
		? int(std::clamp(received * 100 / total, qint64(0), qint64(100)))
		: 0;
	return tr::lng_update_percent(tr::now, lt_value, Lang::Num(value));
}

[[nodiscard]] QString FailureText(
		Updater::Failure failure,
		const QString &version) {
	switch (failure) {
	case Updater::Failure::Signature:
		return tr::lng_update_failed_signature(tr::now);
	case Updater::Failure::Package:
		return tr::lng_update_failed_package(tr::now);
	case Updater::Failure::Unpack:
		return tr::lng_update_failed_unpack(tr::now);
	case Updater::Failure::Disk:
		return tr::lng_update_failed_disk(tr::now);
	case Updater::Failure::Install:
		return tr::lng_update_failed_install(
			tr::now,
			lt_version,
			Lang::Num(version));
	case Updater::Failure::Network:
	case Updater::Failure::None:
		break;
	}
	return tr::lng_update_failed_network(tr::now);
}

// The state in words, a progress bar while something is on its way, when the
// last check was, and one button.
class StatusBlock final : public Ui::RpWidget {
public:
	StatusBlock(QWidget *parent, Updater::Checker *updater)
	: RpWidget(parent)
	, _updater(updater)
	, _button(new Ui::RoundButton(
		this,
		tr::lng_update_check(tr::now),
		st::settingsUpdateButton)) {
		_button->setClickedCallback([this] { act(); });
		connect(updater, &Updater::Checker::stateChanged, this, [this] {
			refresh();
		});
		connect(updater, &Updater::Checker::progressChanged, this, [this] {
			refresh();
		});
		refresh();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::settingsUpdateStatusPadding;
		const auto available = std::max(
			newWidth - padding.left() - padding.right(),
			1);
		auto top = padding.top();
		_titleTop = top;
		top += _title.countHeight(available);
		if (_progress >= 0.) {
			top += st::settingsUpdateBarSkip;
			_barTop = top;
			top += st::settingsUpdateBarHeight;
			top += st::settingsUpdateBarSkip - st::settingsUpdateCaptionSkip;
		}
		top += st::settingsUpdateCaptionSkip;
		_captionTop = top;
		top += _caption.countHeight(available);
		top += st::settingsUpdateButtonSkip;
		_button->resizeToNaturalWidth(available);
		_button->moveToLeft(padding.left(), top, newWidth);
		top += _button->height();
		return top + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::settingsUpdateStatusPadding;
		const auto available = std::max(
			width() - padding.left() - padding.right(),
			1);
		const auto left = padding.left();
		p.setPen(_error ? st::settingsUpdateErrorFg : st::settingsUpdateTitleFg);
		_title.draw(p, left, _titleTop, available);
		if (_progress >= 0.) {
			const auto bar = QRect(
				left,
				_barTop,
				available,
				st::settingsUpdateBarHeight);
			const auto radius = bar.height() / 2.;
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::settingsUpdateBarBg);
			p.drawRoundedRect(bar, radius, radius);
			if (_progress > 0.) {
				const auto filled = std::max(
					int(std::round(bar.width() * _progress)),
					bar.height());
				p.setBrush(st::settingsUpdateBarFg);
				p.drawRoundedRect(
					style::rtlrect(
						bar.x(),
						bar.y(),
						filled,
						bar.height(),
						width()),
					radius,
					radius);
			}
		}
		p.setPen(st::settingsUpdateCaptionFg);
		_caption.draw(p, left, _captionTop, available);
	}

private:
	enum class Action {
		Check,
		Download,
		Restart,
		OpenPage,
		None,
	};

	void act() {
		switch (_action) {
		case Action::Check: _updater->checkNow(); break;
		case Action::Download: _updater->download(); break;
		case Action::Restart: Core::App().restartToUpdate(); break;
		case Action::OpenPage: Core::App().openDownloadPage(); break;
		case Action::None: break;
		}
	}

	void refresh() {
		const auto state = _updater->state();
		const auto &info = _updater->info();
		const auto version = Lang::Num(info.version);
		auto title = QString();
		auto action = Action::Check;
		auto text = tr::lng_update_check(tr::now);
		auto progress = -1.;
		auto busy = false;
		auto error = false;
		switch (state) {
		case Updater::State::Idle:
			title = tr::lng_update_never_checked(tr::now);
			break;
		case Updater::State::Checking:
			title = tr::lng_update_checking(tr::now);
			busy = true;
			break;
		case Updater::State::UpToDate:
			title = tr::lng_update_up_to_date(tr::now);
			break;
		case Updater::State::Available:
			title = (info.size > 0)
				? tr::lng_update_available_size(
					tr::now,
					lt_version,
					version,
					lt_size,
					Lang::Ltr(Lang::Bytes(info.size)))
				: tr::lng_update_available(tr::now, lt_version, version);
			if (_updater->selfUpdate() && info.size > 0) {
				action = Action::Download;
				text = tr::lng_update_download(tr::now);
			} else {
				action = Action::OpenPage;
				text = tr::lng_update_open_page(tr::now);
			}
			break;
		case Updater::State::Downloading: {
			const auto total = _updater->total();
			title = tr::lng_update_downloading(
				tr::now,
				lt_percent,
				Percent(_updater->received(), total),
				lt_size,
				Lang::Ltr(Lang::Bytes(total)));
			progress = (total > 0)
				? std::clamp(double(_updater->received()) / total, 0., 1.)
				: 0.;
			action = Action::None;
			text = tr::lng_update_download(tr::now);
			busy = true;
		} break;
		case Updater::State::Preparing:
			title = tr::lng_update_preparing(tr::now);
			progress = 1.;
			action = Action::None;
			text = tr::lng_update_download(tr::now);
			busy = true;
			break;
		case Updater::State::Ready:
			title = tr::lng_update_ready(tr::now, lt_version, version);
			action = Action::Restart;
			text = tr::lng_update_restart(tr::now);
			break;
		case Updater::State::Failed:
			title = FailureText(_updater->failure(), info.version);
			text = tr::lng_retry(tr::now);
			error = true;
			break;
		}
		const auto checked = _updater->lastChecked();
		const auto caption = !checked.isValid()
			? ((state == Updater::State::Idle)
				? QString()
				: tr::lng_update_never_checked(tr::now))
			: tr::lng_update_last_checked(
				tr::now,
				lt_time,
				Lang::ListTime(checked.toLocalTime()));

		_action = action;
		_error = error;
		_progress = progress;
		_title.setText(st::settingsUpdateTitleStyle, title);
		_caption.setText(st::settingsUpdateCaptionStyle, caption);
		if (_button->text() != text) {
			_button->setText(text);
		}
		_button->setLoading(busy);
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

	Updater::Checker *_updater = nullptr;
	Ui::RoundButton *_button = nullptr;
	Ui::Text::String _title;
	Ui::Text::String _caption;
	Action _action = Action::Check;
	bool _error = false;
	double _progress = -1.;
	int _titleTop = 0;
	int _barTop = 0;
	int _captionTop = 0;

};

// «تازه‌های نسخهٔ …» on a soft surface; no height when there are no notes.
class NotesBlock final : public Ui::RpWidget {
public:
	NotesBlock(QWidget *parent, Updater::Checker *updater)
	: RpWidget(parent)
	, _updater(updater) {
		connect(updater, &Updater::Checker::stateChanged, this, [this] {
			refresh();
		});
		refresh();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_notes.isEmpty()) {
			return 0;
		}
		const auto &margin = st::settingsUpdateNotesMargin;
		const auto &padding = st::settingsUpdateNotesPadding;
		const auto available = std::max(
			newWidth
				- margin.left()
				- margin.right()
				- padding.left()
				- padding.right(),
			1);
		return margin.top()
			+ padding.top()
			+ st::settingsUpdateNotesTitleFont->height
			+ st::settingsUpdateNotesTitleSkip
			+ _notes.countHeight(available)
			+ padding.bottom()
			+ margin.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_notes.isEmpty()) {
			return;
		}
		auto p = Painter(this);
		const auto &margin = st::settingsUpdateNotesMargin;
		const auto &padding = st::settingsUpdateNotesPadding;
		const auto box = rect().marginsRemoved(margin);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::settingsUpdateNotesBg);
			p.drawRoundedRect(
				box,
				st::settingsUpdateNotesRadius,
				st::settingsUpdateNotesRadius);
		}
		const auto inner = box.marginsRemoved(padding);
		p.setFont(st::settingsUpdateNotesTitleFont);
		p.setPen(st::settingsUpdateNotesTitleFg);
		p.drawTextLeft(
			margin.left() + padding.left(),
			inner.y(),
			width(),
			st::settingsUpdateNotesTitleFont->elided(_heading, inner.width()));
		p.setPen(st::settingsUpdateNotesFg);
		// The notes are written by people: each paragraph in its own
		// direction, aligned to the side the interface reads from.
		_notes.draw(p, Ui::Text::PaintContext{
			.position = QPoint(
				inner.x(),
				inner.y()
					+ st::settingsUpdateNotesTitleFont->height
					+ st::settingsUpdateNotesTitleSkip),
			.availableWidth = inner.width(),
			.align = style::al_left,
		});
	}

private:
	void refresh() {
		const auto persian = Lang::IsPersian();
		const auto state = _updater->state();
		const auto offered = (state == Updater::State::Available)
			|| (state == Updater::State::Downloading)
			|| (state == Updater::State::Preparing)
			|| (state == Updater::State::Ready);
		auto version = QString();
		auto notes = QString();
		if (offered) {
			const auto &info = _updater->info();
			version = info.version;
			const auto &first = persian ? info.notesFa : info.notesEn;
			const auto &second = persian ? info.notesEn : info.notesFa;
			notes = first.isEmpty() ? second : first;
		} else {
			// What the running version brought.
			version = _updater->currentVersion();
			notes = _updater->installedNotes(persian);
		}
		notes = notes.trimmed();
		const auto heading = tr::lng_update_notes_title(
			tr::now,
			lt_version,
			Lang::Num(version));
		if (_text == notes && _heading == heading) {
			return;
		}
		_text = notes;
		_heading = heading;
		_notes.setText(st::settingsUpdateNotesStyle, notes);
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

	Updater::Checker *_updater = nullptr;
	Ui::Text::String _notes;
	QString _text;
	QString _heading;

};

class UpdatePage final : public Page {
public:
	UpdatePage(QWidget *parent, Window::SessionController *controller)
	: Page(parent, controller, Section::Update) {
		const auto updater = Core::App().updater();
		if (!updater) {
			showContent();
			return;
		}
		content()->add(Ui::object_ptr<StatusBlock>(content(), updater));
		content()->add(Ui::object_ptr<NotesBlock>(content(), updater));

		AddDivider(content());
		AddInfoRow(
			content(),
			tr::lng_update_current_version(tr::now),
			Lang::Num(updater->currentVersion()));
		if (updater->selfUpdate()) {
			AddToggle(
				content(),
				tr::lng_update_auto(tr::now),
				Core::App().settings().autoUpdate(),
				[updater](bool enabled) {
					Core::App().settings().setAutoUpdate(enabled);
					// Switched on with a version waiting: no second click.
					if (enabled
						&& updater->state() == Updater::State::Available) {
						updater->download();
					}
				});
			AddFootnote(content(), tr::lng_update_auto_about(tr::now));
		} else {
			AddFootnote(content(), tr::lng_update_manual_about(tr::now));
		}
		showContent();
	}

};

} // namespace

Window::SectionFactory UpdatePageFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new UpdatePage(parent, controller));
	};
}

} // namespace Settings
