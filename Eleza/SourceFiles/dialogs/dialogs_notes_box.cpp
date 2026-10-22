#include "dialogs/dialogs_notes_box.h"

#include "data/data_chats.h"
#include "data/data_notes.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "dialogs/dialogs_notes.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>

namespace Dialogs {
namespace {

// The note in a speech bubble over a face (both boxes).
class Stage final : public Ui::RpWidget {
public:
	Stage(QWidget *parent, Main::Session *session, QString userId)
	: RpWidget(parent)
	, _userpics(session, [this](const QString &) { update(); }, this)
	, _userId(std::move(userId)) {
	}

	void setNote(const QString &text, bool placeholder, bool closeFriends) {
		_text.setText(st::dialogsNoteStageTextStyle, text);
		_placeholder = placeholder;
		_closeFriends = closeFriends;
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::dialogsNoteStageTop
			+ bubbleHeight(newWidth)
			+ st::dialogsNoteStageSkip
			+ st::dialogsNoteStageUserpic
			+ st::dialogsNoteStageBottom;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::dialogsNoteStagePadding;
		const auto inner = innerWidth(width());
		const auto lines = linesFor(inner);
		const auto textWidth = std::min(
			_text.countMaxLineWidth(inner, lines),
			inner);
		const auto bubbleWidth = std::max(
			textWidth + padding.left() + padding.right(),
			int(st::dialogsNoteStageMinWidth));
		const auto bubble = QRect(
			(width() - bubbleWidth) / 2,
			st::dialogsNoteStageTop,
			bubbleWidth,
			bubbleHeight(width()));
		PaintNoteBubble(p, bubble, st::dialogsNoteStageRadius, _closeFriends);
		auto geometry = Ui::Text::PaintContext();
		geometry.position = QPoint(
			bubble.x() + padding.left(),
			bubble.y() + padding.top());
		geometry.availableWidth = bubble.width() - padding.left() - padding.right();
		geometry.align = style::al_top;
		geometry.elisionLines = lines;
		geometry.autoDirection = true;
		p.setPen(_placeholder
			? st::dialogsNotesBubbleFgEmpty
			: st::dialogsNotesBubbleFg);
		_text.draw(p, geometry);

		const auto size = int(st::dialogsNoteStageUserpic);
		_userpics.paint(
			p,
			QRect(
				(width() - size) / 2,
				bubble.y() + bubble.height() + st::dialogsNoteStageSkip,
				size,
				size),
			_userId);
	}

private:
	[[nodiscard]] int innerWidth(int width) const {
		const auto &padding = st::dialogsNoteStagePadding;
		return std::max(
			std::min(int(st::dialogsNoteStageMaxWidth), width)
				- padding.left()
				- padding.right(),
			1);
	}
	[[nodiscard]] int linesFor(int inner) const {
		return std::clamp(
			_text.countLines(inner),
			1,
			int(st::dialogsNoteStageLines));
	}
	[[nodiscard]] int bubbleHeight(int width) const {
		const auto &padding = st::dialogsNoteStagePadding;
		return linesFor(innerWidth(width))
			* st::dialogsNoteStageTextStyle.lineHeight
			+ padding.top()
			+ padding.bottom();
	}

	Support::UserpicPainter _userpics;
	QString _userId;
	Ui::Text::String _text;
	bool _placeholder = false;
	bool _closeFriends = false;

};

[[nodiscard]] QString NameOf(const Data::User &user) {
	if (!user.displayName.isEmpty()) {
		return user.displayName;
	}
	return user.handle.isEmpty() ? QString() : Lang::Handle(user.handle);
}

// Refuses a close-friends note while the list is known to be empty (the
// same list as the stories').
[[nodiscard]] bool CloseFriendsReady(Window::SessionController *controller) {
	const auto &settings = controller->session().data().stories().cachedSettings();
	if (settings && settings->closeFriendsCount <= 0) {
		controller->showToast(tr::lng_stories_close_friends_empty(tr::now));
		return false;
	}
	return true;
}

} // namespace

void ShowNoteComposer(Window::SessionController *controller) {
	auto &data = controller->session().data();
	if (!data.notes().enabled()) {
		return;
	}
	if (!data.stories().cachedSettings()) {
		// For the close-friends check of the audience.
		data.stories().settings(controller, nullptr);
	}
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		auto &notes = controller->session().data().notes();
		const auto session = &controller->session();
		const auto current = notes.mine()
			? std::make_optional(*notes.mine())
			: std::nullopt;
		const auto maxChars = notes.maxChars();
		box->setTitle(tr::lng_notes_new(tr::now));

		const auto stage = box->addRow(
			Ui::object_ptr<Stage>(box, session, session->userId()),
			st::boxRowPadding);
		const auto field = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::dialogsNoteField,
			tr::lng_notes_field(tr::now),
			current ? current->text : QString()));
		const auto counter = box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, QString(), st::dialogsNoteCounter),
			st::boxRowPadding,
			style::al_right);
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_notes_audience(tr::now),
			st::boxLabel));
		const auto audience = std::make_shared<Ui::RadiobuttonGroup>(
			int(notes.defaultAudience()));
		box->addRow(Ui::object_ptr<Ui::Radiobutton>(
			box,
			audience,
			int(Data::NoteAudience::Mutuals),
			tr::lng_notes_audience_mutuals(tr::now)));
		box->addRow(Ui::object_ptr<Ui::Radiobutton>(
			box,
			audience,
			int(Data::NoteAudience::CloseFriends),
			tr::lng_notes_audience_close(tr::now)));
		const auto hours = std::max(notes.expirySeconds() / 3600, qint64(1));
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_notes_about(tr::now, lt_count, double(hours)),
			st::captionLabel));

		const auto busy = std::make_shared<bool>(false);
		const auto weak = QPointer<Ui::GenericBox>(box);
		const auto chosen = [=] {
			return Data::NoteAudience(audience->value());
		};
		const auto share = box->addButton(
			tr::lng_notes_share(tr::now),
			nullptr,
			st::defaultBoxButton);
		const auto refresh = [=] {
			const auto text = field->text();
			const auto left = Data::NoteText::Left(text, maxChars);
			counter->setVisible(left <= Data::NoteText::kCounterFrom);
			counter->setText((left >= 0)
				? tr::lng_notes_left(tr::now, lt_count, double(left))
				: tr::lng_notes_over(tr::now, lt_count, double(-left)));
			counter->setTextColorOverride((left < 0)
				? std::make_optional(st::dialogsNoteCounterOverFg->c)
				: std::nullopt);
			const auto clean = Data::NoteText::Cleaned(text);
			stage->setNote(
				clean.isEmpty() ? tr::lng_notes_field(tr::now) : clean,
				clean.isEmpty(),
				chosen() == Data::NoteAudience::CloseFriends);
			const auto unchanged = current
				&& clean == current->text
				&& chosen() == current->audience;
			share->setDisabled(*busy
				|| !Data::NoteText::IsValid(text, maxChars)
				|| unchanged);
		};
		const auto send = [=] {
			if (*busy || share->isDisabled()) {
				return;
			} else if (chosen() == Data::NoteAudience::CloseFriends
				&& !CloseFriendsReady(controller)) {
				return;
			}
			*busy = true;
			share->setLoading(true);
			refresh();
			controller->session().data().notes().set(
				field->text(),
				chosen(),
				box,
				[=](const Data::Note &) {
					controller->showToast(tr::lng_notes_shared(tr::now));
					if (weak) {
						weak->closeBox();
					}
				},
				[=](const Api::Error &error) {
					Support::ShowError(controller, error);
					if (weak) {
						*busy = false;
						share->setLoading(false);
						refresh();
					}
				});
		};
		share->setClickedCallback(send);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		if (current) {
			box->addLeftButton(tr::lng_notes_delete(tr::now), [=] {
				if (*busy) {
					return;
				}
				*busy = true;
				refresh();
				controller->session().data().notes().remove(box, [=] {
					controller->showToast(tr::lng_notes_deleted(tr::now));
					if (weak) {
						weak->closeBox();
					}
				}, [=](const Api::Error &error) {
					Support::ShowError(controller, error);
					if (weak) {
						*busy = false;
						refresh();
					}
				});
			}, st::dangerBoxButton);
		}

		QObject::connect(field, &Ui::InputField::changed, box, [=] {
			// One line, at most the limit: a paste that is too long is cut
			// between characters.
			auto text = field->text();
			text.replace(QLatin1Char('\n'), QLatin1Char(' '));
			const auto fitted = Data::NoteText::Limited(text, maxChars);
			if (fitted != field->text()) {
				field->setText(fitted);
				return; // (changed again)
			}
			refresh();
		});
		QObject::connect(field, &Ui::InputField::submitted, box, [=] {
			send();
		});
		audience->setChangedCallback([=](int) { refresh(); });
		box->setFocusCallback([=] {
			field->setFocusFast();
			field->setCursorToEnd();
		});
		refresh();
	}));
}

void ShowFriendNote(Window::SessionController *controller, const QString &userId) {
	const auto &notes = controller->session().data().notes();
	const auto found = notes.feed().item(userId);
	if (!notes.enabled() || !found) {
		return;
	}
	const auto item = *found;
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		const auto session = &controller->session();
		const auto known = session->data().users().peek(userId);
		const auto card = known.valid() ? known : item.user;
		const auto name = NameOf(card);

		const auto stage = box->addRow(
			Ui::object_ptr<Stage>(box, session, userId),
			st::boxRowPaddingNoTitle);
		stage->setNote(
			item.note.text,
			false,
			item.note.audience == Data::NoteAudience::CloseFriends);
		const auto byline = box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_notes_name_time(
					tr::now,
					lt_name,
					Lang::Isolated(name),
					lt_time,
					Lang::RelativeTime(
						QDateTime::fromSecsSinceEpoch(item.note.createdAt))),
				st::secondaryLabel),
			st::boxRowPadding,
			style::al_top);
		Q_UNUSED(byline);
		const auto field = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::dialogsNoteField,
			tr::lng_notes_message_to(tr::now, lt_name, Lang::Isolated(name))));

		const auto busy = std::make_shared<bool>(false);
		const auto weak = QPointer<Ui::GenericBox>(box);
		const auto send = box->addButton(tr::lng_send(tr::now), nullptr, st::defaultBoxButton);
		const auto refresh = [=] {
			send->setDisabled(*busy || field->text().trimmed().isEmpty());
		};
		const auto submit = [=] {
			const auto answer = field->text().trimmed();
			if (*busy || answer.isEmpty()) {
				return;
			}
			*busy = true;
			send->setLoading(true);
			refresh();
			session->data().notes().reply(
				userId,
				item.note.text,
				answer,
				box,
				[=](const QString &) {
					controller->showToast(tr::lng_notes_sent(tr::now));
					if (weak) {
						weak->closeBox();
					}
				},
				[=](const Api::Error &error) {
					Support::ShowError(controller, error);
					if (weak) {
						*busy = false;
						send->setLoading(false);
						refresh();
					}
				});
		};
		send->setClickedCallback(submit);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		box->addLeftButton(tr::lng_profile_view_profile(tr::now), [=] {
			box->closeBox();
			controller->openRoute(Window::Routes::User(userId));
		});
		QObject::connect(field, &Ui::InputField::changed, box, refresh);
		QObject::connect(field, &Ui::InputField::submitted, box, [=] {
			submit();
		});
		box->setFocusCallback([=] { field->setFocusFast(); });
		refresh();
	}));
}

void DeleteMyNote(Window::SessionController *controller) {
	controller->session().data().notes().remove(controller, [=] {
		controller->showToast(tr::lng_notes_deleted(tr::now));
	}, [=](const Api::Error &error) {
		Support::ShowError(controller, error);
	});
}

void OpenNoteChat(Window::SessionController *controller, const QString &userId) {
	controller->session().data().chats().openDirect(
		userId,
		controller,
		[=](const Data::Chat &chat) {
			// Switches the tab: nothing after this line.
			controller->openRoute(Window::Routes::Chat(chat.id));
		},
		[=](const Api::Error &error) {
			Support::ShowError(controller, error);
		});
}

} // namespace Dialogs
