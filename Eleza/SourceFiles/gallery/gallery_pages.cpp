#include "gallery/gallery_pages.h"

#include "lang/lang_countries.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/shadow.h"
#include "ui/image/image_view.h"
#include "ui/image/userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/chips.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/list_widget.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/fade_wrap.h"
#include "window/window_main_window.h"
#include "styles/style_gallery.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QLinearGradient>

#include <memory>
#include <vector>

namespace Gallery {
namespace {

using Ui::object_ptr;

[[nodiscard]] QString T(const char *fa, const char *en) {
	return QString::fromUtf8(Lang::IsPersian() ? fa : en);
}

// Children in a line, wrapping to the next one when they do not fit.
class FlowRow final : public Ui::RpWidget {
public:
	explicit FlowRow(QWidget *parent, int skip = 10) : RpWidget(parent), _skip(skip) {
	}

	template <typename Widget>
	Widget *add(object_ptr<Widget> child) {
		const auto result = child.release();
		result->setParent(this);
		result->show();
		_children.push_back(result);
		return result;
	}

private:
	static void MoveToLeft(QWidget *child, int x, int y, int outerw) {
		child->move(
			style::RightToLeft() ? (outerw - x - child->width()) : x,
			y);
	}

public:

protected:
	int resizeGetHeight(int newWidth) override {
		auto left = 0;
		auto top = 0;
		auto lineHeight = 0;
		auto lineStart = size_t(0);
		const auto finishLine = [&](size_t till) {
			for (auto i = lineStart; i != till; ++i) {
				const auto child = _children[i];
				child->move(child->x(), top + (lineHeight - child->height()) / 2);
			}
			lineStart = till;
		};
		for (auto i = size_t(0); i != _children.size(); ++i) {
			const auto child = _children[i];
			if (left > 0 && left + child->width() > newWidth) {
				finishLine(i);
				top += lineHeight + _skip;
				left = 0;
				lineHeight = 0;
			}
			MoveToLeft(child, left, top, newWidth);
			left += child->width() + _skip;
			lineHeight = std::max(lineHeight, child->height());
		}
		finishLine(_children.size());
		return top + lineHeight;
	}

private:
	int _skip = 0;
	std::vector<QWidget*> _children;

};

// A rounded surface with rows inside.
class Card final : public Ui::RpWidget {
public:
	explicit Card(QWidget *parent)
	: RpWidget(parent)
	, _content(new Ui::VerticalLayout(this)) {
	}

	[[nodiscard]] Ui::VerticalLayout *content() const {
		return _content;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto skip = st::galleryCardRadius / 2;
		_content->resizeToWidth(newWidth);
		_content->move(0, skip);
		QObject::disconnect(_connection);
		_connection = QObject::connect(
			_content,
			&RpWidget::heightChanged,
			this,
			[this](int height) {
				resize(width(), height + st::galleryCardRadius);
			});
		return _content->height() + 2 * skip;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::galleryCardBorder->c, 1.));
		p.setBrush(st::galleryCardBg);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			st::galleryCardRadius,
			st::galleryCardRadius);
	}

private:
	Ui::VerticalLayout *_content = nullptr;
	QMetaObject::Connection _connection;

};

// Paints with a callback: for one-off drawings.
class Canvas final : public Ui::RpWidget {
public:
	Canvas(
		QWidget *parent,
		int height,
		std::function<void(Painter &p, QSize size)> paint)
	: RpWidget(parent)
	, _paint(std::move(paint)) {
		resize(width(), height);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return height();
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		_paint(p, size());
	}

private:
	std::function<void(Painter &p, QSize size)> _paint;

};

void AddTitle(Ui::VerticalLayout *content, const QString &text) {
	const auto first = (content->count() == 0);
	content->add(
		object_ptr<Ui::FlatLabel>(content, text, st::sectionTitleLabel),
		style::margins(
			0,
			first ? 0 : int(st::gallerySectionSkip),
			0,
			st::galleryTitlePadding.bottom()));
}

FlowRow *AddRow(Ui::VerticalLayout *content, int skip = 10) {
	return content->add(
		object_ptr<FlowRow>(content, skip),
		style::margins(0, 0, 0, st::galleryRowSkip));
}

Ui::VerticalLayout *AddCard(Ui::VerticalLayout *content) {
	return content->add(
		object_ptr<Card>(content),
		style::margins(0, 0, 0, st::galleryRowSkip))->content();
}

[[nodiscard]] QImage SampleImage(int index, QSize size = QSize(480, 320)) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	auto gradient = QLinearGradient(0, 0, size.width(), size.height());
	const QColor pairs[3][2] = {
		{ QColor(0x01, 0x8A, 0x54), QColor(0x7F, 0xDD, 0xA5) },
		{ QColor(0xE0, 0x9A, 0x2B), QColor(0xD6, 0x4F, 0x45) },
		{ QColor(0x5C, 0x8F, 0xD6), QColor(0x8E, 0x7B, 0xD0) },
	};
	gradient.setColorAt(0., pairs[index % 3][0]);
	gradient.setColorAt(1., pairs[index % 3][1]);
	p.fillRect(result.rect(), gradient);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 60));
	p.drawEllipse(QPointF(size.width() * 0.3, size.height() * 0.4), 90, 90);
	p.setBrush(QColor(255, 255, 255, 40));
	p.drawEllipse(QPointF(size.width() * 0.7, size.height() * 0.65), 130, 130);
	return result;
}

// ---- a fake chat list: an example of a Ui::ListDelegate

struct ChatRow {
	QString id;
	QString name;
	QString text;
	QString time;
	int unread = 0;
	bool muted = false;
	bool online = false;
	bool mine = false;
	Ui::UserpicKind kind = Ui::UserpicKind::User;
};

class ChatsDelegate final : public Ui::ListDelegate {
public:
	ChatsDelegate() {
		const auto now = QDateTime::currentDateTime();
		const auto add = [&](
				const char *id,
				QString name,
				QString text,
				qint64 secondsAgo,
				int unread = 0,
				bool muted = false,
				bool online = false,
				Ui::UserpicKind kind = Ui::UserpicKind::User) {
			auto row = ChatRow();
			row.id = QString::fromLatin1(id);
			row.name = std::move(name);
			row.text = std::move(text);
			row.time = Lang::ListTime(now.addSecs(-secondsAgo), now);
			row.unread = unread;
			row.muted = muted;
			row.online = online;
			row.kind = kind;
			_rows.push_back(std::move(row));
		};
		add("u1", T("سارا محمدی", "Sara Mohammadi"),
			T("فردا ساعت ۵ همدیگر را ببینیم؟ ☕", "See you tomorrow at 5? ☕"),
			120, 3, false, true);
		add("saved", tr::lng_saved_messages(tr::now),
			T("فهرست خرید: نان، پنیر، گردو", "Shopping: bread, cheese, walnuts"),
			3600 * 3, 0, false, false, Ui::UserpicKind::Saved);
		add("u2", T("علی رضایی", "Ali Rezaei"),
			tr::lng_dialogs_you(tr::now, lt_text, T("عکس‌ها را فرستادم", "I sent the photos")),
			3600 * 26);
		add("anon", tr::lng_anonymous(tr::now),
			T("ممنون که گوش دادی. خیلی سبک شدم.", "Thanks for listening. I feel lighter."),
			3600 * 50, 12, true, false, Ui::UserpicKind::Anonymous);
		add("u3", QStringLiteral("Design Team"),
			QStringLiteral("Nika: the new icons look great 👌"),
			3600 * 24 * 4, 128);
		add("u4", T("مریم کاظمی", "Maryam Kazemi"),
			tr::lng_action_typing(tr::now),
			3600 * 24 * 9, 0, false, true);
		add("u5", T("کتابخانهٔ کوچک", "The Little Library"),
			T("کتاب تازه رسید: «کلیدر»، جلد سوم", "New arrival: “Kelidar”, volume three"),
			3600 * 24 * 40);
		add("deleted", tr::lng_deleted_account(tr::now),
			T("پیام ذخیره‌شده", "A saved message"),
			qint64(3600) * 24 * 400, 0, false, false, Ui::UserpicKind::Deleted);
	}

	int rowsCount() const override {
		return int(_rows.size());
	}

	int rowHeight(int index, int width) const override {
		Q_UNUSED(index);
		Q_UNUSED(width);
		return st::galleryChatRowHeight;
	}

	void paintRow(
			Painter &p,
			int index,
			const QRect &rect,
			const Ui::ListRowState &state) override {
		const auto &row = _rows[index];
		const auto width = rect.width();
		p.translate(rect.topLeft());

		// Userpic on the leading side.
		const auto userpic = st::galleryChatUserpic;
		const auto padding = st::spaceMd;
		const auto userpicRect = style::rtlrect(
			padding,
			(rect.height() - userpic) / 2,
			userpic,
			userpic,
			width);
		Ui::PaintUserpicPlaceholder(p, userpicRect, row.id, row.name, row.kind);
		if (row.online) {
			Ui::PaintOnlineDot(
				p,
				userpicRect,
				state.active ? st::stateActive : st::bgCanvas);
		}

		const auto left = padding + userpic + padding;
		const auto right = padding;
		const auto nameTop = 13;
		const auto textTop = 37;

		// Time on the trailing side of the first line.
		p.setFont(st::galleryChatTimeFont);
		p.setPen(row.unread ? st::brandText : st::textTertiary);
		const auto timeWidth = st::galleryChatTimeFont->width(row.time);
		p.drawTextRight(right, nameTop + 3, width, row.time, timeWidth);

		const auto nameAvailable = width - left - right - timeWidth - st::spaceSm;
		p.setFont(st::galleryChatNameFont);
		p.setPen((row.kind == Ui::UserpicKind::Anonymous)
			? st::anonContent
			: st::textPrimary);
		const auto name = st::galleryChatNameFont->elided(row.name, nameAvailable);
		p.drawTextLeft(left, nameTop, width, name);

		// Unread badge on the trailing side of the second line.
		auto badgeWidth = 0;
		if (row.unread > 0) {
			badgeWidth = Ui::PaintBadge(
				p,
				Ui::BadgeText(row.unread),
				right,
				textTop - 1,
				width,
				st::defaultBadge,
				row.muted) + st::spaceSm;
		}
		const auto textAvailable = width - left - right - badgeWidth;
		p.setFont(st::galleryChatTextFont);
		p.setPen((row.text == tr::lng_action_typing(tr::now))
			? st::brandPrimary
			: st::textSecondary);
		const auto text = st::galleryChatTextFont->elided(row.text, textAvailable);
		p.drawTextLeft(left, textTop, width, text);
	}

	[[nodiscard]] const ChatRow &row(int index) const {
		return _rows[index];
	}

private:
	std::vector<ChatRow> _rows;

};

void FillSampleMenu(Ui::Menu *menu) {
	menu->addAction(tr::lng_reply(tr::now), nullptr, &st::menuIconReply);
	menu->addAction(tr::lng_copy(tr::now), nullptr, &st::menuIconCopy);
	menu->addAction(tr::lng_forward(tr::now), nullptr, &st::menuIconForward);
	menu->addAction(tr::lng_pin(tr::now), nullptr, &st::menuIconPin);
	auto more = Ui::MenuItem();
	more.text = tr::lng_more(tr::now);
	more.icon = &st::menuIconSettings;
	more.fillSubmenu = [](Ui::PopupMenu *submenu) {
		submenu->addAction(tr::lng_select(tr::now), nullptr, &st::menuIconSelect);
		submenu->addAction(tr::lng_report(tr::now), nullptr, &st::menuIconReport);
	};
	menu->addItem(std::move(more));
	menu->addSeparator();
	auto remove = Ui::MenuItem();
	remove.text = tr::lng_delete(tr::now);
	remove.icon = &st::menuIconDelete;
	remove.destructive = true;
	menu->addItem(std::move(remove));
}

[[nodiscard]] object_ptr<Ui::GenericBox> SampleInputBox(
		Window::MainWindow *window) {
	return Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_profile_edit_profile(tr::now));
		const auto name = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_name(tr::now),
			T("سارا محمدی", "Sara Mohammadi")));
		const auto username = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_username(tr::now),
			QStringLiteral("sara.m")));
		username->setForcedDirection(Qt::LeftToRight);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_intro_username_rules(tr::now),
				st::captionLabel),
			style::margins(28, 0, 28, 8));
		const auto about = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_profile_about_me(tr::now)));
		about->setMaxLength(160);
		box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::lng_settings_private_account(tr::now),
				false),
			st::boxCheckboxPadding);
		box->setFocusCallback([=] { name->setFocusFast(); });
		box->addButton(tr::lng_eli_save(tr::now), [=] {
			if (name->text().trimmed().isEmpty()) {
				name->showError();
				return;
			}
			box->closeBox();
			window->showToast(tr::lng_profile_profile_updated(tr::now));
		}, st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	});
}

class SampleSheet final : public Ui::BoxContent {
public:
	explicit SampleSheet(QWidget *parent) : BoxContent(parent) {
	}

protected:
	void prepare() override {
		setTitle(tr::lng_settings_notifications(tr::now));
		auto content = object_ptr<Ui::VerticalLayout>(this);
		const auto layout = content.data();
		const auto toggle = [&](const QString &text, bool checked) {
			const auto row = layout->add(object_ptr<Ui::SettingsButton>(layout, text));
			row->setToggle(checked);
		};
		layout->add(
			object_ptr<Ui::FlatLabel>(
				layout,
				tr::lng_tab_messages(tr::now),
				st::sectionTitleLabel),
			style::margins(20, 16, 20, 6));
		toggle(tr::lng_settings_notifications(tr::now), true);
		toggle(tr::lng_eli_sound(tr::now), true);
		toggle(tr::lng_chat_tap_reveal(tr::now), false);
		layout->add(object_ptr<Ui::Divider>(layout, st::sectionDivider));
		layout->add(
			object_ptr<Ui::FlatLabel>(
				layout,
				tr::lng_tab_eli(tr::now),
				st::sectionTitleLabel),
			style::margins(20, 8, 20, 6));
		toggle(tr::lng_eli_replies(tr::now), true);
		toggle(tr::lng_eli_mentions(tr::now), true);
		toggle(tr::lng_eli_follow_requests(tr::now), false);
		setInnerWidget(std::move(content));
		addButton(tr::lng_close(tr::now), [this] { closeBox(); });
	}

};

} // namespace

void FillButtonsPage(Ui::VerticalLayout *content, const Context &context) {
	const auto window = context.window;

	AddTitle(content, T("دکمه‌های گرد", "Round buttons"));
	{
		const auto row = AddRow(content);
		const auto primary = row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_send(tr::now), st::defaultRoundButton));
		primary->setClickedCallback([=] {
			window->showToast(tr::lng_chat_sent(tr::now));
		});
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_follow(tr::now), st::lightRoundButton));
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_edit(tr::now), st::outlineRoundButton));
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_delete(tr::now), st::dangerRoundButton));
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_block_user(tr::now), st::dangerLightButton));
		row->add(object_ptr<Ui::FlatButton>(row, tr::lng_cancel(tr::now)));
	}
	{
		const auto row = AddRow(content);
		const auto withIcon = row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_send(tr::now), st::defaultRoundButton));
		withIcon->setIcon(&st::galleryButtonIcon);
		const auto lightIcon = row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_eli_find_people(tr::now), st::lightRoundButton));
		lightIcon->setIcon(&st::galleryLightButtonIcon);
		const auto loading = row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_continue(tr::now), st::defaultRoundButton));
		loading->setLoading(true);
		const auto disabled = row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_continue(tr::now), st::defaultRoundButton));
		disabled->setDisabled(true);
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_intro_get_code(tr::now), st::bigRoundButton));
	}
	{
		const auto row = AddRow(content);
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_follow(tr::now), st::smallRoundButton));
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_following(tr::now), st::smallLightButton));
		row->add(object_ptr<Ui::RoundButton>(
			row, tr::lng_follow_requested(tr::now), st::smallOutlineButton));
		row->add(object_ptr<Ui::LinkButton>(row, tr::lng_intro_forgot_password(tr::now)));
		row->add(object_ptr<Ui::LinkButton>(
			row, tr::lng_intro_resend(tr::now), st::smallLinkButton));
	}

	AddTitle(content, T("دکمه‌های نمادین", "Icon buttons"));
	{
		const auto row = AddRow(content, 4);
		row->add(object_ptr<Ui::IconButton>(row, st::topBarBackButton));
		row->add(object_ptr<Ui::IconButton>(row, st::topBarSearchButton));
		row->add(object_ptr<Ui::IconButton>(row, st::topBarCallButton));
		const auto bell = row->add(object_ptr<Ui::IconButton>(row, st::topBarBellButton));
		bell->setDotShown(true);
		row->add(object_ptr<Ui::IconButton>(row, st::topBarCreateButton));
		row->add(object_ptr<Ui::IconButton>(row, st::topBarEditButton));
		row->add(object_ptr<Ui::IconButton>(row, st::topBarSettingsButton));
		row->add(object_ptr<Ui::IconButton>(row, st::topBarMoreButton));
		row->add(object_ptr<Ui::IconButton>(row, st::brandIconButton));
		const auto off = row->add(object_ptr<Ui::IconButton>(row, st::topBarCloseButton));
		off->setDisabled(true);
		row->add(object_ptr<Ui::IconButton>(row, st::smallIconButton));
		row->add(object_ptr<Ui::IconButton>(row, st::smallCloseButton));
	}

	AddTitle(content, T("کلیدها و گزینه‌ها", "Toggles and choices"));
	{
		const auto row = AddRow(content, 18);
		row->add(object_ptr<Ui::Toggle>(row, true));
		row->add(object_ptr<Ui::Toggle>(row, false));
		const auto off = row->add(object_ptr<Ui::Toggle>(row, true));
		off->setDisabled(true);
		row->add(object_ptr<Ui::Checkbox>(
			row, tr::lng_report_also_block(tr::now), true));
		row->add(object_ptr<Ui::Checkbox>(
			row, tr::lng_chat_delete_both(tr::now), false));
	}
	{
		const auto row = AddRow(content, 18);
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(1);
		row->add(object_ptr<Ui::Radiobutton>(
			row, group, 0, tr::lng_theme_automatic(tr::now)));
		row->add(object_ptr<Ui::Radiobutton>(
			row, group, 1, tr::lng_theme_light(tr::now)));
		row->add(object_ptr<Ui::Radiobutton>(
			row, group, 2, tr::lng_theme_dark(tr::now)));
	}

	AddTitle(content, T("تراشه‌ها و زبانه‌ها", "Chips and tabs"));
	{
		const auto chips = content->add(
			object_ptr<Ui::ChipsRow>(content),
			style::margins(-16, 0, -16, 0));
		chips->addChip(QStringLiteral("all"), tr::lng_all(tr::now));
		chips->addChip(QStringLiteral("people"), tr::lng_search_people(tr::now));
		chips->addChip(QStringLiteral("posts"), tr::lng_search_posts(tr::now));
		chips->addChip(
			QStringLiteral("talks"),
			tr::lng_search_conversations(tr::now));
		chips->addChip(QStringLiteral("topics"), tr::lng_conv_topics(tr::now));
		chips->addChip(QStringLiteral("media"), tr::lng_profile_media(tr::now));
		chips->addChip(
			QStringLiteral("hot"),
			tr::lng_search_hot_hashtags(tr::now),
			&st::galleryChipIcon);
		chips->addChip(
			QStringLiteral("videos"),
			tr::lng_search_new_videos(tr::now));
		chips->addChip(
			QStringLiteral("known"),
			tr::lng_search_people_may_know(tr::now));
		chips->setActive(QStringLiteral("all"), anim::type::instant);

		const auto tabs = content->add(
			object_ptr<Ui::TabBar>(content),
			style::margins(-8, 0, -8, st::galleryRowSkip));
		tabs->setTabs({
			tr::lng_eli_for_you(tr::now),
			tr::lng_following(tr::now),
			tr::lng_eli_trending(tr::now),
			tr::lng_eli_bookmarks_title(tr::now),
		});
		tabs->setTabBadge(2, true);

		const auto row = AddRow(content, 8);
		const auto pill = row->add(object_ptr<Ui::PillButton>(
			row, tr::lng_eli_most_relevant(tr::now), st::smallPillButton));
		pill->setActive(true, false);
		const auto second = row->add(object_ptr<Ui::PillButton>(
			row, tr::lng_eli_newest(tr::now), st::smallPillButton));
		pill->setClickedCallback([=] {
			pill->setActive(true);
			second->setActive(false);
		});
		second->setClickedCallback([=] {
			pill->setActive(false);
			second->setActive(true);
		});
	}

	AddTitle(content, T("ردیف‌های تنظیمات", "Settings rows"));
	{
		const auto card = AddCard(content);
		const auto add = [&](
				const QString &text,
				const style::icon *icon,
				const QString &value = QString()) {
			const auto row = card->add(object_ptr<Ui::SettingsButton>(card, text));
			row->setIcon(icon);
			row->setChevron(true);
			if (!value.isEmpty()) {
				row->setValueText(value);
			}
			return row;
		};
		add(tr::lng_settings_account(tr::now), &st::settingsIconAccount);
		add(tr::lng_settings_privacy(tr::now), &st::settingsIconPrivacy);
		add(
			tr::lng_settings_language(tr::now),
			&st::settingsIconLanguage,
			Lang::NativeName(Lang::Current()));
		add(
			tr::lng_settings_appearance(tr::now),
			&st::settingsIconAppearance,
			style::IsDark()
				? tr::lng_theme_dark(tr::now)
				: tr::lng_theme_light(tr::now));
		card->add(object_ptr<Ui::Divider>(card, st::insetDivider));
		const auto notify = card->add(object_ptr<Ui::SettingsButton>(
			card, tr::lng_settings_notifications(tr::now)));
		notify->setIcon(&st::settingsIconNotifications);
		notify->setToggle(true);
		const auto region = card->add(object_ptr<Ui::SettingsButton>(
			card, tr::lng_settings_eli_region(tr::now)));
		region->setIcon(&st::settingsIconRegion);
		region->setToggle(false);
		card->add(object_ptr<Ui::Divider>(card, st::insetDivider));
		const auto out = card->add(object_ptr<Ui::SettingsButton>(
			card, tr::lng_sign_out(tr::now)));
		out->setIcon(&st::settingsIconLogout);
		out->setDestructive(true);
	}

	AddTitle(content, T("نشان‌ها", "Badges"));
	{
		const auto row = AddRow(content, 12);
		for (const auto count : { 1, 7, 42, 128 }) {
			const auto badge = row->add(object_ptr<Ui::UnreadBadge>(row));
			badge->setCount(count);
		}
		const auto muted = row->add(object_ptr<Ui::UnreadBadge>(row));
		muted->setCount(5);
		muted->setMuted(true);
		const auto small = row->add(object_ptr<Ui::UnreadBadge>(row, st::smallBadge));
		small->setCount(3);
	}
}

void FillInputsPage(Ui::VerticalLayout *content, const Context &context) {
	Q_UNUSED(context);
	const auto fieldMargins = style::margins(0, 0, 0, st::galleryRowSkip);

	AddTitle(content, T("فیلد متن", "Text field"));
	content->add(
		object_ptr<Ui::InputField>(
			content, st::defaultInputField, tr::lng_name(tr::now)),
		fieldMargins);
	content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			tr::lng_name(tr::now),
			T("سارا محمدی", "Sara Mohammadi")),
		fieldMargins);
	const auto username = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			tr::lng_username(tr::now),
			QStringLiteral("sara")),
		style::margins(0, 0, 0, 4));
	username->setForcedDirection(Qt::LeftToRight);
	username->showError();
	username->clearFocus();
	const auto hint = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_intro_username_taken(tr::now),
			st::captionLabel),
		style::margins(4, 0, 4, st::galleryRowSkip));
	hint->setTextColorOverride(st::statusError->c);

	AddTitle(content, T("فیلد چندخطی", "Multi-line field"));
	const auto about = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_profile_about_me(tr::now),
			T(
				"عاشق کتاب، چای و گفتگوهای طولانی.\n"
				"Designer · Tehran · she/her",
				"Books, tea and long conversations.\n"
				"طراح · تهران")),
		fieldMargins);
	about->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);

	AddTitle(content, T("جستجو", "Search"));
	content->add(
		object_ptr<Ui::SearchField>(
			content, tr::lng_search_search_people_topics_posts(tr::now)),
		fieldMargins);

	AddTitle(content, T("شمارهٔ موبایل", "Phone number"));
	const auto phone = content->add(
		object_ptr<Ui::PhoneInput>(content),
		fieldMargins);
	phone->setNumberPlaceholder(QStringLiteral("912 345 6789"));
	const auto filled = content->add(
		object_ptr<Ui::PhoneInput>(content),
		fieldMargins);
	filled->setNumber(QStringLiteral("9123456789"));

	AddTitle(content, tr::lng_intro_verification_code(tr::now));
	{
		const auto row = AddRow(content, 24);
		const auto code = row->add(object_ptr<Ui::CodeInput>(row, 5));
		code->setCode(QStringLiteral("284"));
		const auto wrong = row->add(object_ptr<Ui::CodeInput>(row, 5));
		wrong->setCode(QStringLiteral("11111"));
		wrong->showError();
	}

	AddTitle(content, tr::lng_intro_two_step_password(tr::now));
	const auto password = content->add(
		object_ptr<Ui::PasswordInput>(
			content, st::defaultInputField, tr::lng_intro_password(tr::now)),
		fieldMargins);
	password->setText(QStringLiteral("correct horse"));

	AddTitle(content, T("فقط رقم", "Digits only"));
	content->add(
		object_ptr<Ui::DigitsInput>(
			content,
			st::defaultInputField,
			T("سال تولد", "Birth year"),
			QStringLiteral("1375"),
			4),
		fieldMargins);
}

void FillTextPage(Ui::VerticalLayout *content, const Context &context) {
	const auto window = context.window;
	const auto labelMargins = style::margins(0, 0, 0, st::galleryRowSkip);

	AddTitle(content, T("مقیاس حروف", "Type scale"));
	content->add(object_ptr<Ui::FlatLabel>(
		content, tr::lng_app_name(tr::now), st::displayLabel));
	content->add(
		object_ptr<Ui::FlatLabel>(
			content, tr::lng_intro_tagline(tr::now), st::largeTitleLabel),
		labelMargins);
	content->add(object_ptr<Ui::FlatLabel>(
		content, tr::lng_conv_what_do_want_talk_about(tr::now), st::titleLabel));
	content->add(object_ptr<Ui::FlatLabel>(
		content, tr::lng_intro_profile_title(tr::now), st::subtitleLabel));
	content->add(object_ptr<Ui::FlatLabel>(
		content, tr::lng_conv_one_sentence_enough_listener_only(tr::now)));
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_intro_terms(tr::now),
			st::captionLabel),
		labelMargins);

	AddTitle(content, T("متن چندخطی و دوجهته", "Multi-line and bidirectional"));
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			T(
				"الزا جایی برای گفتن و شنیدن است. می‌توانی با دوستانت گپ بزنی، "
				"در «الی» بنویسی یا یک گفتگوی ناشناس باز کنی تا کسی فقط بشنود. "
				"نسخهٔ دسکتاپ با Qt 6 و C++20 ساخته شده و روی Windows و Linux "
				"اجرا می‌شود؛ شماره‌ها مثل ۱۴۰۴/۰۷/۱۷ و 2025-10-09 هم درست می‌نشینند.",
				"Eleza is a place to speak and to listen. Chat with friends, "
				"write on «الی» (Eli) or open an anonymous conversation so that "
				"someone just listens. The desktop app is built with Qt 6 and "
				"C++20; Persian words like گفتگو and dates like ۱۴۰۴/۰۷/۱۷ sit "
				"correctly inside English text."),
			st::bodyLargeLabel),
		labelMargins);

	AddTitle(content, T("قالب‌بندی، پیوندها و اسپویلر", "Entities, links and spoilers"));
	{
		// Offsets are UTF-16 units, exactly as the server sends them.
		const auto text = T(
			"پررنگ، کج و زیرخط‌دار؛ کد: git pull؛ پیوند eleza.me، "
			"نام‌بردن @sara.m و برچسب #کتاب_خوب 📚. "
			"پایان داستان: قاتل باغبان بود! این هم یک پیوند با متن دلخواه.",
			"Bold, italic and underlined; code: git pull; a link eleza.me, "
			"a mention @sara.m and a hashtag #good_books 📚. "
			"The ending: the gardener did it! And a link with custom text.");
		auto marked = Ui::Text::ParseEntities(text);
		const auto add = [&](
				Ui::Text::EntityType type,
				const QString &part,
				const QString &data = QString()) {
			const auto offset = int(text.indexOf(part));
			if (offset >= 0) {
				marked.entities.push_back(Ui::Text::Entity{
					type,
					offset,
					int(part.size()),
					data,
				});
			}
		};
		add(Ui::Text::EntityType::Bold, T("پررنگ", "Bold"));
		add(Ui::Text::EntityType::Italic, T("کج", "italic"));
		add(Ui::Text::EntityType::Underline, T("زیرخط‌دار", "underlined"));
		add(Ui::Text::EntityType::Code, QStringLiteral("git pull"));
		add(
			Ui::Text::EntityType::Spoiler,
			T("قاتل باغبان بود!", "the gardener did it!"));
		add(
			Ui::Text::EntityType::CustomUrl,
			T("پیوند با متن دلخواه", "link with custom text"),
			QStringLiteral("https://eleza.me/about"));
		const auto label = content->add(
			object_ptr<Ui::FlatLabel>(content, st::bodyLargeLabel),
			labelMargins);
		label->setMarkedText(marked);
		label->setSelectable(true);
		label->setLinkHandler([=](const Ui::Text::Link &link, Qt::MouseButton) {
			window->showToast(link.data);
			return true;
		});
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				T(
					"متن بالا انتخاب‌شدنی است؛ روی اسپویلر کلیک کن تا دیده شود.",
					"The text above is selectable; click the spoiler to reveal it."),
				st::captionLabel),
			labelMargins);
	}

	AddTitle(content, T("کوتاه‌سازی", "Elision"));
	{
		const auto longText = T(
			"این متن عمداً طولانی است تا در دو خط کوتاه شود و سه‌نقطه در "
			"انتهای خط دوم دیده شود؛ ادامهٔ متن هر چه باشد دیگر نمایش داده "
			"نمی‌شود و فقط با باز کردن پست می‌توان آن را خواند. این جمله هم "
			"برای اطمینان از سرریز شدن اضافه شده است.",
			"This text is long on purpose so that it is cut to two lines with "
			"an ellipsis at the end of the second one; whatever follows is not "
			"shown and can only be read by opening the post. This sentence is "
			"here just to make sure the text overflows.");
		const auto two = content->add(
			object_ptr<Ui::FlatLabel>(content, longText),
			labelMargins);
		two->setMaxLines(2);
		const auto one = content->add(
			object_ptr<Ui::FlatLabel>(content, longText, st::secondaryLabel),
			labelMargins);
		one->setMaxLines(1);
	}

	AddTitle(content, T("حروف نمایشی (مرکزی)", "Display type (Markazi Text)"));
	content->add(
		object_ptr<Canvas>(content, 150, [](Painter &p, QSize size) {
			auto top = 0;
			const auto line = [&](const style::font &font, const QString &text) {
				p.setFont(font);
				p.setPen(st::textPrimary);
				p.drawTextLeft(0, top, size.width(), text);
				top += font->height + 2;
			};
			line(st::topicFontL, T(
				"امروز از چه می‌خواهی حرف بزنی؟",
				"What do you want to talk about today?"));
			line(st::editorialFontQuote, T(
				"«گاهی فقط لازم است کسی بشنود.»",
				"“Sometimes you only need someone to listen.”"));
			line(st::topicFontS, T("دلتنگی شب‌های پاییز", "Autumn evenings"));
			line(st::numericFontL, Lang::Num(QStringLiteral("0123456789")));
		}),
		labelMargins);

	AddTitle(content, T("تاریخ و عدد", "Dates and numbers"));
	{
		const auto now = QDateTime::currentDateTime();
		auto lines = QStringList();
		lines.push_back(Lang::SolarDate(now.date(), true)
			+ QStringLiteral("  ·  ")
			+ Lang::Weekday(now.date())
			+ QStringLiteral("  ·  ")
			+ Lang::Clock(now));
		lines.push_back(Lang::LastSeen(QString(), now.toSecsSinceEpoch() - 5400, now)
			+ QStringLiteral("  ·  ")
			+ Lang::RelativeTime(now.addSecs(-1500), now)
			+ QStringLiteral("  ·  ")
			+ Lang::ListTime(now.addDays(-3), now));
		lines.push_back(Lang::CompactCount(987)
			+ QStringLiteral("  ·  ")
			+ Lang::CompactCount(1250)
			+ QStringLiteral("  ·  ")
			+ Lang::CompactCount(48300)
			+ QStringLiteral("  ·  ")
			+ Lang::CompactCount(3400000)
			+ QStringLiteral("  ·  ")
			+ Lang::Duration(3723)
			+ QStringLiteral("  ·  ")
			+ Lang::Bytes(3565158));
		lines.push_back(tr::lng_eli_new_posts(tr::now, lt_count, 1)
			+ QStringLiteral("  ·  ")
			+ tr::lng_eli_new_posts(tr::now, lt_count, 12)
			+ QStringLiteral("  ·  ")
			+ tr::lng_conv_hours_left(tr::now, lt_count, 23));
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				lines.join(QLatin1Char('\n')),
				st::secondaryLabel),
			labelMargins);
	}
}

void FillListsPage(Ui::VerticalLayout *content, const Context &context) {
	const auto window = context.window;

	AddTitle(content, T("فهرست مجازی (ردیف‌های گفتگو)", "Virtual list (chat rows)"));
	{
		const auto card = AddCard(content);
		// The delegate lives as long as the list.
		const auto delegate = std::make_shared<ChatsDelegate>();
		const auto list = card->add(object_ptr<Ui::ListWidget>(card, delegate.get()));
		QObject::connect(list, &QObject::destroyed, [delegate] {});
		list->setActiveRow(2);
		QObject::connect(
			list,
			&Ui::ListWidget::rowClicked,
			list,
			[=](int index) { list->setActiveRow(index); });
		QObject::connect(
			list,
			&Ui::ListWidget::rowContextMenu,
			list,
			[=](int index, QPoint globalPosition) {
				const auto menu = new Ui::PopupMenu(list);
				const auto muted = delegate->row(index).muted;
				menu->addAction(
					tr::lng_chat_mark_as_read(tr::now),
					nullptr,
					&st::menuIconSelect);
				menu->addAction(tr::lng_pin(tr::now), nullptr, &st::menuIconPin);
				menu->addAction(
					muted
						? tr::lng_chat_unmute(tr::now)
						: tr::lng_chat_mute(tr::now),
					nullptr,
					muted ? &st::menuIconUnmute : &st::menuIconMute);
				menu->addAction(
					tr::lng_chat_archive(tr::now),
					nullptr,
					&st::menuIconArchive);
				menu->addSeparator();
				auto remove = Ui::MenuItem();
				remove.text = tr::lng_chat_delete_chat(tr::now);
				remove.icon = &st::menuIconDelete;
				remove.destructive = true;
				remove.callback = [=] {
					auto args = Ui::ConfirmBoxArgs();
					args.text = tr::lng_chat_delete_chat_both_sides(tr::now);
					args.confirmText = tr::lng_delete(tr::now);
					args.destructive = true;
					window->show(Ui::MakeConfirmBox(std::move(args)));
				};
				menu->addItem(std::move(remove));
				menu->popup(globalPosition);
			});
	}

	AddTitle(content, T("تصویر نمایه", "Userpics"));
	{
		const auto row = AddRow(content, 14);
		const auto names = std::vector<std::pair<QString, QString>>{
			{ QStringLiteral("u1"), T("سارا", "Sara") },
			{ QStringLiteral("u2"), T("علی", "Ali") },
			{ QStringLiteral("u7"), T("نیکا", "Nika") },
			{ QStringLiteral("u9"), QStringLiteral("Design") },
			{ QStringLiteral("u12"), T("مریم", "Maryam") },
			{ QStringLiteral("u15"), T("بهار", "Bahar") },
			{ QStringLiteral("u21"), QStringLiteral("🦊 Fox") },
		};
		auto index = 0;
		for (const auto &[id, name] : names) {
			const auto pic = row->add(object_ptr<Ui::UserpicWidget>(row, 48));
			pic->setPlaceholder(id, name);
			pic->setOnline((index++ % 3) == 0);
		}
		const auto photo = row->add(object_ptr<Ui::UserpicWidget>(row, 48));
		photo->setImage(SampleImage(0, QSize(200, 200)));
		photo->setOnline(true);
		const auto anonymous = row->add(object_ptr<Ui::UserpicWidget>(row, 48));
		anonymous->setPlaceholder(
			QStringLiteral("anon"),
			QString(),
			Ui::UserpicKind::Anonymous);
		const auto saved = row->add(object_ptr<Ui::UserpicWidget>(row, 48));
		saved->setPlaceholder(
			QStringLiteral("saved"),
			QString(),
			Ui::UserpicKind::Saved);
	}
	{
		const auto row = AddRow(content, 14);
		for (const auto size : { 88, 56, 40, 32, 24 }) {
			const auto pic = row->add(object_ptr<Ui::UserpicWidget>(row, size));
			pic->setPlaceholder(QStringLiteral("u3"), T("الزا", "Eleza"));
		}
	}

	AddTitle(content, T("تصویرها", "Images"));
	{
		const auto row = AddRow(content, 12);
		const auto plain = row->add(object_ptr<Ui::ImageView>(row));
		plain->resize(180, 120);
		plain->setRadius(st::radiusMd);
		plain->setImage(SampleImage(0));
		const auto bubble = row->add(object_ptr<Ui::ImageView>(row));
		bubble->resize(180, 120);
		bubble->setRadius(18, 18, 4, 18);
		bubble->setSource(Ui::MakeStaticImageSource(SampleImage(1)));
		const auto blurred = row->add(object_ptr<Ui::ImageView>(row));
		blurred->resize(180, 120);
		blurred->setRadius(st::radiusMd);
		blurred->setImage(SampleImage(2));
		blurred->setBlurred(true);
		// A source that never delivers: the loading skeleton.
		class Pending final : public Ui::ImageSource {
		public:
			QImage request(QSize) override {
				return QImage();
			}
		};
		const auto loading = row->add(object_ptr<Ui::ImageView>(row));
		loading->resize(120, 120);
		loading->setRadius(st::radiusMd);
		loading->setSource(std::make_shared<Pending>());
	}

	AddTitle(content, T("در حال بارگذاری", "Loading skeletons"));
	{
		const auto card = AddCard(content);
		card->add(object_ptr<Ui::SkeletonList>(
			card, Ui::SkeletonList::Kind::Chats, 3));
		card->add(object_ptr<Ui::Divider>(card));
		card->add(object_ptr<Ui::SkeletonList>(
			card, Ui::SkeletonList::Kind::Posts, 1));
	}
}

void FillLayersPage(Ui::VerticalLayout *content, const Context &context) {
	const auto window = context.window;
	const auto body = window->bodyWidget();

	const auto showConfirm = [=] {
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_sign_out(tr::now);
		args.text = tr::lng_settings_sign_out_about(tr::now);
		args.confirmText = tr::lng_sign_out(tr::now);
		args.confirmed = [=](std::function<void()> close) {
			close();
			window->showToast(tr::lng_app_session_has_ended_sign_again(tr::now));
		};
		window->show(Ui::MakeConfirmBox(std::move(args)));
	};
	const auto showDestructive = [=] {
		auto args = Ui::ConfirmBoxArgs();
		args.text = tr::lng_eli_delete_post_sure(tr::now);
		args.confirmText = tr::lng_delete(tr::now);
		args.destructive = true;
		args.checkboxText = tr::lng_report_also_block(tr::now);
		args.confirmed = [=](std::function<void()> close) {
			close();
			window->showToast(tr::lng_eli_post_deleted(tr::now));
		};
		window->show(Ui::MakeConfirmBox(std::move(args)));
	};
	const auto showBox = [=] {
		window->show(SampleInputBox(window));
	};
	const auto showSheet = [=] {
		window->showSheet(Ui::Box<SampleSheet>());
	};
	const auto showToast = [=] {
		auto config = Ui::Toast::Config();
		config.text = tr::lng_link_copied(tr::now);
		config.icon = &st::toastIconCheck;
		Ui::Toast::Show(body, std::move(config));
	};

	AddTitle(content, T("جعبه‌ها و برگه‌ها", "Boxes and sheets"));
	{
		const auto row = AddRow(content);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("جعبهٔ فرم", "Form box"), st::lightRoundButton)
		)->setClickedCallback(showBox);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("تأیید", "Confirm"), st::lightRoundButton)
		)->setClickedCallback(showConfirm);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("تأیید حذف", "Destructive confirm"), st::lightRoundButton)
		)->setClickedCallback(showDestructive);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("برگهٔ کناری", "Side sheet"), st::lightRoundButton)
		)->setClickedCallback(showSheet);
	}

	AddTitle(content, T("اعلان کوتاه", "Toasts"));
	{
		const auto row = AddRow(content);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("با نماد", "With an icon"), st::outlineRoundButton)
		)->setClickedCallback(showToast);
		row->add(object_ptr<Ui::RoundButton>(
			row, T("چندخطی", "Multi-line"), st::outlineRoundButton)
		)->setClickedCallback([=] {
			window->showToast(tr::lng_conv_joined_will_appear_chats_shortly(tr::now)
				+ QLatin1Char(' ')
				+ tr::lng_conv_well_tell_right_here_when(tr::now));
		});
		row->add(object_ptr<Ui::RoundButton>(
			row, T("سه تا پشت هم", "Three in a row"), st::outlineRoundButton)
		)->setClickedCallback([=] {
			window->showToast(tr::lng_copied(tr::now));
			window->showToast(tr::lng_eli_saved_toast(tr::now));
			window->showToast(tr::lng_eli_reposted(tr::now));
		});
	}

	AddTitle(content, T("منوها", "Menus"));
	Ui::RoundButton *dropdownButton = nullptr;
	Ui::RoundButton *popupButton = nullptr;
	{
		const auto row = AddRow(content);
		popupButton = row->add(object_ptr<Ui::RoundButton>(
			row, T("منوی بازشو", "Popup menu"), st::outlineRoundButton));
		const auto popupAnchor = popupButton;
		popupButton->setClickedCallback([=] {
			const auto menu = new Ui::PopupMenu(popupAnchor);
			FillSampleMenu(menu->menu());
			menu->popupAnchored(popupAnchor, style::al_left);
		});
		dropdownButton = row->add(object_ptr<Ui::RoundButton>(
			row, T("منوی درون‌پنجره", "In-window dropdown"), st::outlineRoundButton));
		const auto anchor = dropdownButton;
		dropdownButton->setClickedCallback([=] {
			const auto menu = new Ui::DropdownMenu(body);
			menu->setDestroyOnHide(true);
			FillSampleMenu(menu->menu());
			menu->showAnchored(anchor, style::al_left);
		});
		row->add(object_ptr<Ui::FlatLabel>(
			row,
			T("یا هر جای این صفحه راست‌کلیک کن", "or right-click the rows on the Lists page"),
			st::captionLabel));
	}
	{
		// The list of a menu as it looks inside a popup.
		const auto holder = content->add(
			object_ptr<Canvas>(content, 290, [](Painter &p, QSize size) {
				const auto &st = st::defaultPopupMenu;
				const auto menuWidth = 220;
				const auto body = style::rtlrect(
					st.shadow.extend.left(),
					st.shadow.extend.top(),
					menuWidth,
					size.height()
						- st.shadow.extend.top()
						- st.shadow.extend.bottom(),
					size.width());
				Ui::Shadow::Paint(p, body, st.radius, st.shadow);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(QPen(st.borderFg->c, 1.));
				p.setBrush(st.bg);
				p.drawRoundedRect(
					QRectF(body).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
					st.radius,
					st.radius);
			}),
			style::margins(-18, 0, 0, st::galleryRowSkip));
		const auto menu = new Ui::Menu(holder);
		FillSampleMenu(menu);
		menu->resizeToWidth(220);
		menu->setSelected(1);
		const auto &extend = st::defaultPopupMenu.shadow.extend;
		const auto place = [=] {
			menu->moveToLeft(extend.left(), extend.top(), holder->width());
			holder->resize(
				holder->width(),
				menu->height() + extend.top() + extend.bottom());
		};
		QObject::connect(holder, &Ui::RpWidget::widthChanged, menu, place);
		place();
	}

	AddTitle(content, T("راهنمای ابزار", "Tooltips"));
	{
		const auto row = AddRow(content);
		const auto button = row->add(object_ptr<Ui::IconButton>(
			row, st::topBarSettingsButton));
		Ui::InstallTooltip(button, [] { return tr::lng_settings(tr::now); });
		const auto text = tr::lng_conv_only_one_person_can_listen(tr::now);
		row->add(object_ptr<Canvas>(row, 64, [=](Painter &p, QSize size) {
			const auto pixmap = Ui::Tooltip::Render(
				text,
				p.device()->devicePixelRatioF());
			p.drawPixmapLeft(0, 0, size.width(), pixmap);
		}))->resize(290, 64);
	}

	// Screenshot helper: --debug-route=<overlay>.
	const auto overlay = context.overlay;
	if (!overlay.isEmpty()) {
		const auto anchorDropdown = dropdownButton;
		Ui::PostponeCall(content, [=] {
			if (overlay == QStringLiteral("box")) {
				showBox();
			} else if (overlay == QStringLiteral("confirm")) {
				showConfirm();
			} else if (overlay == QStringLiteral("destructive")) {
				showDestructive();
			} else if (overlay == QStringLiteral("sheet")) {
				showSheet();
			} else if (overlay == QStringLiteral("toast")) {
				showToast();
			} else if (overlay == QStringLiteral("dropdown")
				|| overlay == QStringLiteral("menu")) {
				anchorDropdown->click();
			}
		});
	}
}

void FillMiscPage(Ui::VerticalLayout *content, const Context &context) {
	const auto window = context.window;

	AddTitle(content, T("حالت خالی", "Empty state"));
	{
		const auto card = AddCard(content);
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		descriptor.icon = &st::emptyIconChats;
		descriptor.title = tr::lng_dialogs_empty_title(tr::now);
		descriptor.text = tr::lng_dialogs_empty_text(tr::now);
		descriptor.buttonText = tr::lng_dialogs_new_message(tr::now);
		descriptor.buttonCallback = [=] {
			window->showToast(tr::lng_dialogs_new_message(tr::now));
		};
		card->add(object_ptr<Ui::EmptyPlaceholder>(card, std::move(descriptor)));
	}

	AddTitle(content, T("بارگذاری", "Progress"));
	{
		const auto row = AddRow(content, 20);
		row->add(object_ptr<Ui::LoadingSpinner>(row, 24, st::brandPrimary));
		row->add(object_ptr<Ui::LoadingSpinner>(row, 32, st::textTertiary));
		row->add(object_ptr<Ui::LoadingSpinner>(row, 44, st::accentLamp));
		row->add(object_ptr<Canvas>(row, 44, [](Painter &p, QSize) {
			static auto radial = Ui::RadialProgressAnimation(nullptr);
			radial.setProgress(0.62, false);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::brandSoft);
			p.drawEllipse(0, 0, 44, 44);
			radial.draw(p, QRect(5, 5, 34, 34), 3., st::brandPrimary->c);
		}))->resize(44, 44);
	}

	AddTitle(content, T("سایه‌ها", "Elevation"));
	content->add(
		object_ptr<Canvas>(content, 130, [](Painter &p, QSize size) {
			const auto draw = [&](int index, const style::Shadow &shadow, int radius) {
				const auto rect = style::rtlrect(
					24 + index * 190,
					26,
					150,
					64,
					size.width());
				Ui::Shadow::Paint(p, rect, radius, shadow);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::bgRaised);
				p.drawRoundedRect(rect, radius, radius);
				p.setFont(st::smallSemiboldFont);
				p.setPen(st::textSecondary);
				p.drawTextCentered(
					rect,
					QStringLiteral("Elevation / %1").arg(index + 1));
			};
			draw(0, st::smallShadow, st::radiusMd);
			draw(1, st::defaultShadow, st::radiusMd);
			draw(2, st::boxShadow, st::radiusLg);
		}),
		style::margins(0, 0, 0, st::galleryRowSkip));

	AddTitle(content, T("رنگ‌ها", "Palette"));
	{
		const auto palette = style::internal::GeneratedPalette();
		const auto cell = st::gallerySwatchSize;
		const auto count = palette.count;
		const auto holder = content->add(
			object_ptr<Canvas>(content, 100, [=](Painter &p, QSize size) {
				const auto columns = std::max(size.width() / (cell.width() + 8), 1);
				auto hq = PainterHighQualityEnabler(p);
				for (auto i = 0; i != count; ++i) {
					const auto x = (i % columns) * (cell.width() + 8);
					const auto y = (i / columns) * (cell.height() + 8);
					const auto rect = style::rtlrect(
						x, y, cell.width(), cell.height(), size.width());
					const auto swatch = QRect(rect.x(), rect.y(), rect.width(), 30);
					p.setPen(QPen(st::borderSubtle->c, 1.));
					p.setBrush(palette.data[i].c);
					p.drawRoundedRect(
						QRectF(swatch).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
						8,
						8);
					p.setFont(st::microFont);
					p.setPen(st::textSecondary);
					const auto name = st::microFont->elided(
						QString::fromLatin1(palette.rows[i].name),
						rect.width());
					p.drawText(
						rect.x() + (rect.width() - st::microFont->width(name)) / 2,
						rect.y() + 34 + st::microFont->ascent,
						name);
				}
			}),
			style::margins(0, 0, 0, st::galleryRowSkip));
		const auto relayout = [=] {
			const auto columns = std::max(holder->width() / (cell.width() + 8), 1);
			const auto rows = (count + columns - 1) / columns;
			holder->resize(holder->width(), rows * (cell.height() + 8));
		};
		QObject::connect(holder, &Ui::RpWidget::widthChanged, holder, relayout);
		relayout();
	}

	AddTitle(content, T("نمادها", "Icons"));
	{
		auto names = QDir(QStringLiteral(":/icons")).entryList(
			{ QStringLiteral("*.svg") },
			QDir::Files,
			QDir::Name);
		for (auto &name : names) {
			name.chop(4);
		}
		const auto cell = st::galleryIconCell;
		const auto holder = content->add(
			object_ptr<Canvas>(content, 100, [=](Painter &p, QSize size) {
				const auto columns = std::max(size.width() / cell.width(), 1);
				const auto ratio = p.device()->devicePixelRatioF();
				for (auto i = 0; i != names.size(); ++i) {
					const auto rect = style::rtlrect(
						(i % columns) * cell.width(),
						(i / columns) * cell.height(),
						cell.width(),
						cell.height(),
						size.width());
					p.drawPixmap(
						rect.x() + (rect.width() - 24) / 2,
						rect.y() + 8,
						style::IconPixmap(names[i], st::textPrimary->c, 24, ratio));
					p.setFont(st::microFont);
					p.setPen(st::textTertiary);
					const auto name = st::microFont->elided(
						names[i],
						rect.width() - 6);
					p.drawText(
						rect.x() + (rect.width() - st::microFont->width(name)) / 2,
						rect.y() + 38 + st::microFont->ascent,
						name);
				}
			}));
		const auto count = int(names.size());
		const auto relayout = [=] {
			const auto columns = std::max(holder->width() / cell.width(), 1);
			const auto rows = (count + columns - 1) / columns;
			holder->resize(holder->width(), rows * cell.height());
		};
		QObject::connect(holder, &Ui::RpWidget::widthChanged, holder, relayout);
		relayout();
	}

	AddTitle(content, T("کشورها", "Countries"));
	{
		auto lines = QStringList();
		const auto &countries = Lang::Countries();
		for (auto i = 0; i != 6 && i != int(countries.size()); ++i) {
			lines.push_back(countries[i].name()
				+ QStringLiteral("  +")
				+ Lang::Num(countries[i].code));
		}
		content->add(object_ptr<Ui::FlatLabel>(
			content,
			lines.join(QStringLiteral("   ·   ")),
			st::secondaryLabel));
	}
}

} // namespace Gallery
