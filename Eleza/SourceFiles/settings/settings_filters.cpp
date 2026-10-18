// «بی‌صدا و مسدود»: muted words, blocked accounts, muted accounts, hidden
// accounts, hidden topics, people blocked in masked conversations; and the
// lists of the privacy page: last seen exceptions and sent follow requests.
#include "settings/settings_pages.h"

#include "boxes/peer_list_box.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/ui_utility.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using Controller = Window::SessionController;
using Changed = std::function<void()>;

constexpr auto kMaxWordLength = 64;
constexpr auto kMaxExceptionPages = 20;

// A person with one small action on the trailing side.
class PeerRow final : public Ui::RpWidget {
public:
	PeerRow(
		QWidget *parent,
		Main::Session *session,
		const Data::User &user,
		const QString &actionText,
		std::function<void()> action,
		std::function<void()> open)
	: RpWidget(parent)
	, _name(user.deleted
		? tr::lng_deleted_account(tr::now)
		: user.displayName)
	, _handle(user.handle.isEmpty()
		? QString()
		: Lang::Handle(user.handle))
	, _userpic(new Support::UserpicButton(
		this,
		session,
		user.id,
		st::settingsPeerUserpic))
	, _action(new Ui::RoundButton(this, actionText, st::smallOutlineButton)) {
		_userpic->setClickedCallback(std::move(open));
		_action->setClickedCallback(std::move(action));
	}

	void setBusy(bool busy) {
		_action->setLoading(busy);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::settingsPeerPadding;
		_userpic->moveToLeft(
			padding.left(),
			(st::settingsPeerHeight - st::settingsPeerUserpic) / 2,
			newWidth);
		_action->resizeToNaturalWidth(newWidth / 2);
		_action->moveToRight(
			padding.right(),
			(st::settingsPeerHeight - _action->height()) / 2,
			newWidth);
		return st::settingsPeerHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::settingsPeerPadding;
		const auto left = padding.left()
			+ st::settingsPeerUserpic
			+ st::settingsPeerSkip;
		const auto available = std::max(
			width()
				- left
				- padding.right()
				- _action->width()
				- st::settingsPeerButtonSkip,
			0);
		const auto single = _handle.isEmpty();
		p.setFont(st::settingsPeerNameFont);
		p.setPen(st::settingsPeerNameFg);
		p.drawTextLeft(
			left,
			single
				? ((height() - st::settingsPeerNameFont->height) / 2)
				: int(st::settingsPeerNameTop),
			width(),
			st::settingsPeerNameFont->elided(_name, available));
		if (!single) {
			p.setFont(st::settingsPeerStatusFont);
			p.setPen(st::settingsPeerStatusFg);
			p.drawTextLeft(
				left,
				st::settingsPeerStatusTop,
				width(),
				st::settingsPeerStatusFont->elided(_handle, available));
		}
	}

private:
	QString _name;
	QString _handle;
	Support::UserpicButton *_userpic = nullptr;
	Ui::RoundButton *_action = nullptr;

};

// A line of text with a trailing control (a muted word, a hidden topic).
class TextRow final : public Ui::RpWidget {
public:
	TextRow(QWidget *parent, const QString &text)
	: RpWidget(parent)
	, _text(text) {
	}

	Ui::IconButton *addRemove(std::function<void()> callback) {
		const auto button = new Ui::IconButton(this, st::settingsWordRemove);
		button->setClickedCallback(std::move(callback));
		setControl(button);
		return button;
	}
	Ui::RoundButton *addAction(
			const QString &text,
			std::function<void()> callback) {
		const auto button = new Ui::RoundButton(
			this,
			text,
			st::smallOutlineButton);
		button->setClickedCallback(std::move(callback));
		setControl(button);
		return button;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_control) {
			_control->resizeToNaturalWidth(newWidth / 2);
			_control->moveToRight(
				st::settingsPeerPadding.right(),
				(st::settingsWordHeight - _control->height()) / 2,
				newWidth);
		}
		return st::settingsWordHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto left = st::settingsPeerPadding.left();
		const auto available = std::max(
			width()
				- left
				- st::settingsPeerPadding.right()
				- (_control ? _control->width() : 0)
				- st::settingsPeerButtonSkip,
			0);
		p.setFont(st::settingsWordFont);
		p.setPen(st::settingsWordFg);
		p.drawTextLeft(
			left,
			(height() - st::settingsWordFont->height) / 2,
			width(),
			st::settingsWordFont->elided(_text, available));
	}

private:
	void setControl(Ui::RpWidget *control) {
		_control = control;
		_control->show();
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

	QString _text;
	Ui::RpWidget *_control = nullptr;

};

// «واژهٔ تازه» + «افزودن».
class AddRow final : public Ui::RpWidget {
public:
	AddRow(
		QWidget *parent,
		const QString &placeholder,
		std::function<void(const QString &)> add)
	: RpWidget(parent)
	, _field(new Ui::InputField(this, st::defaultInputField, placeholder))
	, _button(new Ui::RoundButton(
		this,
		tr::lng_settings_add(tr::now),
		st::defaultRoundButton))
	, _add(std::move(add)) {
		_field->setMaxLength(kMaxWordLength);
		connect(_field, &Ui::InputField::submitted, this, [this] { submit(); });
		_button->setClickedCallback([this] { submit(); });
	}

	void setFocusFast() {
		_field->setFocusFast();
	}
	void clear() {
		_field->clear();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_button->resizeToNaturalWidth(newWidth / 2);
		const auto fieldWidth = newWidth
			- _button->width()
			- st::settingsAddButtonSkip;
		_field->resizeToWidth(fieldWidth);
		_field->moveToLeft(0, 0, newWidth);
		_button->moveToRight(
			0,
			(_field->height() - _button->height()) / 2,
			newWidth);
		return _field->height();
	}

private:
	void submit() {
		const auto text = _field->text().simplified();
		if (text.isEmpty()) {
			_field->showError();
			return;
		}
		_add(text);
	}

	Ui::InputField *_field = nullptr;
	Ui::RoundButton *_button = nullptr;
	std::function<void(const QString &)> _add;

};

struct ListParts {
	Ui::FlatLabel *empty = nullptr;
	Ui::VerticalLayout *rows = nullptr;

	void refresh() const {
		empty->setVisible(rows->count() == 0);
	}
};

[[nodiscard]] ListParts AddList(Ui::GenericBox *box, const QString &emptyText) {
	auto result = ListParts();
	result.empty = box->addRow(
		Ui::object_ptr<Ui::FlatLabel>(box, emptyText, st::centeredLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::space2xl,
			st::boxRowPadding.right(),
			st::space2xl));
	result.rows = box->verticalLayout()->add(
		Ui::object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
	result.refresh();
	return result;
}

// ---- muted words

void ShowMutedWordsBox(
		Controller *controller,
		const QStringList &words,
		Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_muted_words(tr::now));
		const auto graph = &controller->session().data().graph();
		const auto known = std::make_shared<QStringList>();
		const auto parts = std::make_shared<ListParts>();
		const auto addRow = std::make_shared<Fn<void(const QString &)>>();
		*addRow = [=](const QString &word) {
			known->push_back(word);
			const auto row = parts->rows->add(
				Ui::object_ptr<TextRow>(parts->rows, word));
			row->addRemove([=] {
				graph->removeMutedWord(word, box, changed);
				known->removeAll(word);
				// The button that was clicked dies with the row.
				Ui::PostponeCall(box, [=] {
					parts->rows->removeChild(row);
					parts->refresh();
				});
			});
			parts->refresh();
		};
		const auto input = std::make_shared<QPointer<AddRow>>();
		*input = box->addRow(
			Ui::object_ptr<AddRow>(
				box,
				tr::lng_settings_new_word(tr::now),
				[=](const QString &word) {
					if (!known->contains(word, Qt::CaseInsensitive)) {
						graph->addMutedWord(word, box, changed);
						(*addRow)(word);
					}
					if (*input) {
						(*input)->clear();
					}
				}),
			st::settingsAddFieldPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_muted_words_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);
		*parts = AddList(box, tr::lng_settings_list_empty(tr::now));
		for (const auto &word : words) {
			(*addRow)(word);
		}
		box->setFocusCallback([=] {
			if (*input) {
				(*input)->setFocusFast();
			}
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- blocked accounts

void ShowBlockedBox(Controller *controller) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_blocked(tr::now));
		const auto session = &controller->session();
		const auto parts = std::make_shared<ListParts>(
			AddList(box, tr::lng_loading(tr::now)));
		struct State {
			QString cursor;
			bool loading = false;
			bool finished = false;
		};
		const auto state = std::make_shared<State>();
		const auto load = std::make_shared<Fn<void()>>();
		*load = [=] {
			if (state->loading || state->finished) {
				return;
			}
			state->loading = true;
			session->data().graph().blocks(state->cursor, box, [=](
					const Data::GraphList &list) {
				state->loading = false;
				state->cursor = list.nextCursor;
				state->finished = list.nextCursor.isEmpty()
					|| list.users.isEmpty();
				for (const auto &user : list.users) {
					const auto id = user.id;
					const auto name = user.displayName;
					const auto row = std::make_shared<QPointer<PeerRow>>();
					*row = parts->rows->add(Ui::object_ptr<PeerRow>(
						parts->rows,
						session,
						user,
						tr::lng_unblock(tr::now),
						[=] {
							if (*row) {
								(*row)->setBusy(true);
							}
							session->data().graph().unblock(id, box, [=](
									const Data::GraphState &) {
								if (*row) {
									parts->rows->removeChild(row->data());
								}
								parts->refresh();
								controller->showToast(
									tr::lng_settings_unblocked_toast(
										tr::now,
										lt_name,
										name));
							}, [=](const Api::Error &error) {
								if (*row) {
									(*row)->setBusy(false);
								}
								Support::ShowError(controller, error);
							});
						},
						[=] {
							const auto route = Window::Routes::User(id);
							box->closeBox();
							controller->openRoute(route);
						}));
				}
				parts->empty->setText(tr::lng_settings_list_empty(tr::now));
				parts->refresh();
			}, [=](const Api::Error &error) {
				state->loading = false;
				state->finished = true;
				parts->empty->setText(Support::ErrorText(
					error,
					tr::lng_error_load(tr::now)));
				parts->refresh();
			});
		};
		(*load)();
		// The scroll area exists once the box is put together.
		Ui::PostponeCall(box, [=] {
			if (const auto scroll = box->scrollArea()) {
				QObject::connect(
					scroll,
					&Ui::ScrollArea::nearBottom,
					box,
					[=] { (*load)(); });
			}
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- muted accounts

void ShowMutedBox(
		Controller *controller,
		const QStringList &ids,
		Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_muted(tr::now));
		const auto session = &controller->session();
		const auto parts = std::make_shared<ListParts>(AddList(
			box,
			ids.isEmpty()
				? tr::lng_settings_list_empty(tr::now)
				: tr::lng_loading(tr::now)));
		const auto users = &session->data().users();
		users->ensure(ids, box, [=] {
			for (const auto &id : ids) {
				const auto user = users->peek(id);
				if (!user.valid()) {
					continue;
				}
				const auto name = user.displayName;
				const auto row = std::make_shared<QPointer<PeerRow>>();
				*row = parts->rows->add(Ui::object_ptr<PeerRow>(
					parts->rows,
					session,
					user,
					tr::lng_settings_unmute_user(tr::now),
					[=] {
						session->data().graph().unmute(id);
						controller->showToast(tr::lng_settings_unmuted_toast(
							tr::now,
							lt_name,
							name));
						Ui::PostponeCall(box, [=] {
							if (*row) {
								parts->rows->removeChild(row->data());
							}
							parts->refresh();
							if (changed) {
								changed();
							}
						});
					},
					[=] {
						const auto route = Window::Routes::User(id);
						box->closeBox();
						controller->openRoute(route);
					}));
			}
			parts->empty->setText(tr::lng_settings_list_empty(tr::now));
			parts->refresh();
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- hidden topics

void ShowHiddenTopicsBox(
		Controller *controller,
		const QVector<Data::Filters::HiddenTopic> &topics,
		Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_hidden_topics(tr::now));
		const auto graph = &controller->session().data().graph();
		const auto parts = std::make_shared<ListParts>(
			AddList(box, tr::lng_settings_list_empty(tr::now)));
		for (const auto &topic : topics) {
			const auto id = topic.id;
			const auto title = !topic.title.isEmpty()
				? topic.title
				: !topic.slug.isEmpty()
				? topic.slug
				: topic.id;
			const auto row = parts->rows->add(Ui::object_ptr<TextRow>(
				parts->rows,
				QLatin1Char('#') + title));
			row->addAction(tr::lng_settings_show_again(tr::now), [=] {
				graph->unhideTopic(id, box, changed);
				controller->showToast(
					tr::lng_settings_topic_shown_toast(tr::now));
				Ui::PostponeCall(box, [=] {
					parts->rows->removeChild(row);
					parts->refresh();
				});
			});
		}
		parts->refresh();
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- hidden accounts

void ShowHiddenUsersBox(
		Controller *controller,
		const QStringList &ids,
		Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_hidden_users(tr::now));
		const auto session = &controller->session();
		const auto parts = std::make_shared<ListParts>(AddList(
			box,
			ids.isEmpty()
				? tr::lng_settings_list_empty(tr::now)
				: tr::lng_loading(tr::now)));
		const auto users = &session->data().users();
		users->ensure(ids, box, [=] {
			for (const auto &id : ids) {
				auto user = users->peek(id);
				if (!user.valid()) {
					user.id = id;
				}
				const auto name = user.displayName;
				const auto row = std::make_shared<QPointer<PeerRow>>();
				*row = parts->rows->add(Ui::object_ptr<PeerRow>(
					parts->rows,
					session,
					user,
					tr::lng_settings_show_again(tr::now),
					[=] {
						session->data().graph().unhide(id);
						controller->showToast(tr::lng_settings_unhidden_toast(
							tr::now,
							lt_name,
							name));
						Ui::PostponeCall(box, [=] {
							if (*row) {
								parts->rows->removeChild(row->data());
							}
							parts->refresh();
							if (changed) {
								changed();
							}
						});
					},
					[=] {
						const auto route = Window::Routes::User(id);
						box->closeBox();
						controller->openRoute(route);
					}));
			}
			parts->empty->setText(tr::lng_settings_list_empty(tr::now));
			parts->refresh();
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- blocked in masked conversations

void ShowContactBansBox(Controller *controller) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_contact_bans(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_contact_bans_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);
		const auto graph = &controller->session().data().graph();
		const auto parts = std::make_shared<ListParts>(
			AddList(box, tr::lng_loading(tr::now)));
		graph->contactBans(box, [=](const QVector<Data::ContactBan> &bans) {
			for (const auto &ban : bans) {
				const auto token = ban.token;
				const auto name = ban.label.trimmed().isEmpty()
					? tr::lng_settings_contact_bans_unnamed(tr::now)
					: ban.label.simplified();
				const auto row = std::make_shared<QPointer<TextRow>>();
				*row = parts->rows->add(
					Ui::object_ptr<TextRow>(parts->rows, name));
				const auto button = (*row)->addAction(tr::lng_unblock(tr::now), nullptr);
				const auto weakButton = QPointer<Ui::RoundButton>(button);
				button->setClickedCallback([=] {
					if (weakButton) {
						weakButton->setLoading(true);
					}
					graph->removeContactBan(token, box, [=] {
						controller->showToast(tr::lng_settings_unblocked_toast(
							tr::now,
							lt_name,
							name));
						Ui::PostponeCall(box, [=] {
							if (*row) {
								parts->rows->removeChild(row->data());
							}
							parts->refresh();
						});
					}, [=](const Api::Error &error) {
						if (weakButton) {
							weakButton->setLoading(false);
						}
						Support::ShowError(controller, error);
					});
				});
			}
			parts->empty->setText(tr::lng_settings_list_empty(tr::now));
			parts->refresh();
		}, [=](const Api::Error &error) {
			parts->empty->setText(Support::ErrorText(
				error,
				tr::lng_error_load(tr::now)));
			parts->refresh();
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- people for a list: searched on the server, several at once

class PickPeopleController final : public Boxes::PeerListController {
public:
	PickPeopleController(
		QStringList exclude,
		Fn<void(QStringList)> chosen)
	: _exclude(std::move(exclude))
	, _chosen(std::move(chosen)) {
	}

	QString title() const override {
		return tr::lng_settings_ls_add(tr::now);
	}
	QString searchPlaceholder() const override {
		return tr::lng_settings_ls_search(tr::now);
	}
	QString emptyText() const override {
		return delegate()->searchQuery().trimmed().size() < 2
			? tr::lng_settings_ls_search(tr::now)
			: tr::lng_nothing_found(tr::now);
	}
	bool searchRemotely() const override {
		return true;
	}
	bool multiSelect() const override {
		return true;
	}
	QString submitText() const override {
		return tr::lng_settings_add(tr::now);
	}
	void prepare() override {
		delegate()->setRows({});
	}
	void rowClicked(const Boxes::PeerListRow &row) override {
		Q_UNUSED(row);
	}
	void searchQueryChanged(const QString &query) override {
		const auto trimmed = query.trimmed();
		const auto generation = ++_generation;
		if (trimmed.size() < 2) {
			delegate()->setRows({});
			return;
		}
		delegate()->setLoading(true);
		const auto session = this->session();
		const auto me = session->userId();
		session->data().graph().searchPeople(
			trimmed,
			base::Guard(delegate()->peerListBox()),
			[=, this](const QVector<Data::Profile> &people) {
				if (generation != _generation) {
					return;
				}
				auto rows = QVector<Boxes::PeerListRow>();
				for (const auto &person : people) {
					if (person.id.isEmpty()
						|| person.id == me
						|| person.deleted
						|| person.isBot) {
						continue;
					}
					auto row = Boxes::PeerListRow::FromUser(person);
					if (_exclude.contains(person.id)) {
						row.disabled = true;
					}
					rows.push_back(row);
				}
				delegate()->setRows(rows);
			});
	}
	void submit(const QStringList &ids) override {
		const auto chosen = _chosen;
		delegate()->closeBox();
		if (chosen) {
			chosen(ids);
		}
	}

private:
	QStringList _exclude;
	Fn<void(QStringList)> _chosen;
	int _generation = 0;

};

// ---- sent follow requests

void ShowSentRequestsBoxImpl(Controller *controller) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_sent_requests(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_sent_requests_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);
		const auto session = &controller->session();
		const auto parts = std::make_shared<ListParts>(
			AddList(box, tr::lng_loading(tr::now)));
		struct State {
			QString cursor;
			bool loading = false;
			bool finished = false;
		};
		const auto state = std::make_shared<State>();
		const auto load = std::make_shared<Fn<void()>>();
		*load = [=] {
			if (state->loading || state->finished) {
				return;
			}
			state->loading = true;
			session->data().graph().sentFollowRequests(state->cursor, box, [=](
					const Data::GraphList &list) {
				state->loading = false;
				state->cursor = list.nextCursor;
				state->finished = list.nextCursor.isEmpty()
					|| list.users.isEmpty();
				for (const auto &user : list.users) {
					const auto id = user.id;
					const auto name = user.displayName;
					const auto row = std::make_shared<QPointer<PeerRow>>();
					*row = parts->rows->add(Ui::object_ptr<PeerRow>(
						parts->rows,
						session,
						user,
						tr::lng_settings_sent_cancel(tr::now),
						[=] {
							if (*row) {
								(*row)->setBusy(true);
							}
							session->data().graph().unfollow(id, box, [=](
									const Data::GraphState &) {
								if (*row) {
									parts->rows->removeChild(row->data());
								}
								parts->refresh();
								controller->showToast(
									tr::lng_settings_sent_cancelled(
										tr::now,
										lt_name,
										name));
							}, [=](const Api::Error &error) {
								if (*row) {
									(*row)->setBusy(false);
								}
								Support::ShowError(controller, error);
							});
						},
						[=] {
							const auto route = Window::Routes::User(id);
							box->closeBox();
							controller->openRoute(route);
						}));
				}
				parts->empty->setText(tr::lng_settings_list_empty(tr::now));
				parts->refresh();
			}, [=](const Api::Error &error) {
				state->loading = false;
				state->finished = true;
				parts->empty->setText(Support::ErrorText(
					error,
					tr::lng_error_load(tr::now)));
				parts->refresh();
			});
		};
		(*load)();
		Ui::PostponeCall(box, [=] {
			if (const auto scroll = box->scrollArea()) {
				QObject::connect(
					scroll,
					&Ui::ScrollArea::nearBottom,
					box,
					[=] { (*load)(); });
			}
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- last seen exceptions

void ShowLastSeenExceptionsBoxImpl(
		Controller *controller,
		const QString &kind,
		Changed changed) {
	const auto always = (kind == u"always");
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(always
			? tr::lng_settings_ls_always(tr::now)
			: tr::lng_settings_ls_never(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				always
					? tr::lng_settings_ls_always_about(tr::now)
					: tr::lng_settings_ls_never_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);
		const auto session = &controller->session();
		const auto graph = &session->data().graph();
		const auto known = std::make_shared<QStringList>();
		const auto parts = std::make_shared<ListParts>();
		const auto addPerson = std::make_shared<Fn<void(const Data::User &)>>();
		*addPerson = [=](const Data::User &user) {
			if (user.id.isEmpty() || known->contains(user.id)) {
				return;
			}
			known->push_back(user.id);
			const auto id = user.id;
			const auto name = user.displayName;
			const auto row = std::make_shared<QPointer<PeerRow>>();
			*row = parts->rows->add(Ui::object_ptr<PeerRow>(
				parts->rows,
				session,
				user,
				tr::lng_settings_ls_remove(tr::now),
				[=] {
					if (*row) {
						(*row)->setBusy(true);
					}
					graph->removeLastSeenException(id, box, [=] {
						known->removeAll(id);
						controller->showToast(tr::lng_settings_ls_removed(
							tr::now,
							lt_name,
							name));
						if (*row) {
							parts->rows->removeChild(row->data());
						}
						parts->refresh();
						if (changed) {
							changed();
						}
					}, [=](const Api::Error &error) {
						if (*row) {
							(*row)->setBusy(false);
						}
						Support::ShowError(controller, error);
					});
				},
				[=] {
					const auto route = Window::Routes::User(id);
					box->closeBox();
					controller->openRoute(route);
				}));
			parts->refresh();
		};
		const auto add = box->addRow(
			Ui::object_ptr<Ui::SettingsButton>(
				box,
				tr::lng_settings_ls_add(tr::now),
				st::settingsButton),
			style::margins());
		add->setIcon(&st::settingsIconAddPeople);
		add->setClickedCallback([=] {
			const auto weak = QPointer<Ui::GenericBox>(box);
			controller->show(Ui::Box<Boxes::PeerListBox>(
				session,
				std::make_unique<PickPeopleController>(*known, [=](
						const QStringList &ids) {
					for (const auto &id : ids) {
						graph->setLastSeenException(id, kind, box, [=] {
							if (!weak) {
								return;
							}
							auto user = session->data().users().peek(id);
							if (!user.valid()) {
								user.id = id;
							}
							(*addPerson)(user);
							controller->showToast(tr::lng_settings_ls_added(
								tr::now,
								lt_name,
								Support::MemberName(session, id)));
							if (changed) {
								changed();
							}
						}, [=](const Api::Error &error) {
							Support::ShowError(controller, error);
						});
					}
				})));
		});
		*parts = AddList(box, tr::lng_loading(tr::now));

		const auto load = std::make_shared<Fn<void(QString, int)>>();
		*load = [=](const QString &cursor, int page) {
			graph->lastSeenExceptions(kind, cursor, box, [=](
					const Data::LastSeenExceptionsPage &result) {
				for (const auto &user : result.users) {
					(*addPerson)(user);
				}
				if (!result.nextCursor.isEmpty() && page < kMaxExceptionPages) {
					(*load)(result.nextCursor, page + 1);
				} else {
					parts->empty->setText(tr::lng_settings_list_empty(tr::now));
					parts->refresh();
				}
			}, [=](const Api::Error &error) {
				parts->empty->setText(Support::ErrorText(
					error,
					tr::lng_error_load(tr::now)));
				parts->refresh();
			});
		};
		(*load)(QString(), 0);
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

class FiltersPage final : public Page {
public:
	FiltersPage(QWidget *parent, Controller *controller)
	: Page(parent, controller, Section::Filters) {
		load(true);
	}

private:
	void load(bool first) {
		if (first) {
			showLoading();
		}
		controller()->session().data().graph().filters(this, [this](
				const Data::Filters &filters) {
			_filters = filters;
			rebuild();
		}, [=, this](const Api::Error &error) {
			if (first) {
				showError(error, [this] { load(true); });
			}
		});
	}

	void rebuild() {
		const auto window = controller();
		const auto reload = Guarded(this, [this] { load(false); });
		const auto count = [](qsizetype value) {
			return value ? Lang::Num(qint64(value)) : QString();
		};
		clearContent();
		const auto filters = _filters;
		AddLinkButton(
			content(),
			tr::lng_settings_muted_words(tr::now),
			&st::settingsIconWords,
			[=] { ShowMutedWordsBox(window, filters.mutedWords, reload); }
		)->setValueText(count(filters.mutedWords.size()));
		AddLinkButton(
			content(),
			tr::lng_settings_blocked(tr::now),
			&st::settingsIconMuted,
			[=] { ShowBlockedBox(window); });
		AddLinkButton(
			content(),
			tr::lng_settings_muted(tr::now),
			&st::settingsIconVolume,
			[=] { ShowMutedBox(window, filters.muted, reload); }
		)->setValueText(count(filters.muted.size()));
		AddLinkButton(
			content(),
			tr::lng_settings_hidden_users(tr::now),
			&st::settingsIconHidden,
			[=] { ShowHiddenUsersBox(window, filters.hidden, reload); }
		)->setValueText(count(filters.hidden.size()));
		AddLinkButton(
			content(),
			tr::lng_settings_contact_bans(tr::now),
			&st::settingsIconAnonymous,
			[=] { ShowContactBansBox(window); });
		AddLinkButton(
			content(),
			tr::lng_settings_hidden_topics(tr::now),
			&st::settingsIconTopics,
			[=] { ShowHiddenTopicsBox(window, filters.hiddenTopics, reload); }
		)->setValueText(count(filters.hiddenTopics.size()));
		AddFootnote(content(), tr::lng_settings_filters_about(tr::now));
		showContent();
	}

	Data::Filters _filters;

};

} // namespace

void ShowLastSeenExceptionsBox(
		Window::SessionController *controller,
		const QString &kind,
		std::function<void()> changed) {
	ShowLastSeenExceptionsBoxImpl(controller, kind, std::move(changed));
}

void ShowSentRequestsBox(Window::SessionController *controller) {
	ShowSentRequestsBoxImpl(controller);
}

Window::SectionFactory FiltersPageFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new FiltersPage(parent, controller));
	};
}

} // namespace Settings
