// The small boxes of «الی»: who liked / disliked a post, and the report form
// with the reasons the server offers.
#include "eli/eli_actions.h"

#include "boxes/peer_list_box.h"
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

constexpr auto kReportNoteLength = 500;

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

[[nodiscard]] QString ReasonLabel(const QString &slug) {
	if (slug == u"spam") {
		return tr::lng_eli_reason_spam(tr::now);
	} else if (slug == u"harassment") {
		return tr::lng_eli_reason_harassment(tr::now);
	} else if (slug == u"hate") {
		return tr::lng_eli_reason_hate(tr::now);
	} else if (slug == u"violence") {
		return tr::lng_eli_reason_violence(tr::now);
	} else if (slug == u"sexual") {
		return tr::lng_eli_reason_sexual(tr::now);
	} else if (slug == u"self_harm") {
		return tr::lng_eli_reason_self_harm(tr::now);
	} else if (slug == u"misinformation") {
		return tr::lng_eli_reason_misinformation(tr::now);
	} else if (slug == u"impersonation") {
		return tr::lng_eli_reason_impersonation(tr::now);
	} else if (slug == u"other") {
		return tr::lng_eli_reason_other(tr::now);
	}
	// A reason this build has no wording for: its slug, readable.
	auto readable = slug;
	readable.replace(QChar('_'), QChar(' '));
	return readable;
}

[[nodiscard]] QStringList DefaultReasons() {
	return {
		u"spam"_s,
		u"harassment"_s,
		u"hate"_s,
		u"violence"_s,
		u"sexual"_s,
		u"self_harm"_s,
		u"misinformation"_s,
		u"impersonation"_s,
		u"other"_s,
	};
}

[[nodiscard]] QString Question(const QString &kind) {
	if (kind == u"reply") {
		return tr::lng_eli_report_q_reply(tr::now);
	} else if (kind == u"account") {
		return tr::lng_eli_report_q_account(tr::now);
	} else if (kind == u"conversation") {
		return tr::lng_eli_report_q_conversation(tr::now);
	}
	return tr::lng_eli_report_q_post(tr::now);
}

void ShowReportBox(
		Window::SessionController *controller,
		const ReportTarget &target,
		const QStringList &reasons) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_report(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, Question(target.kind), st::boxLabel),
			st::boxRowPadding);
		const auto group = std::make_shared<Ui::RadiobuttonGroup>();
		for (auto i = 0; i != int(reasons.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(box, group, i, ReasonLabel(reasons[i])),
				st::boxCheckboxPadding);
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_eli_report_anonymous(tr::now),
				st::captionLabel),
			st::boxRowPadding);
		const auto note = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_report_details(tr::now)));
		note->setMaxLength(kReportNoteLength);
		note->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		const auto offerBlock = !target.offerBlockUserId.isEmpty()
			&& (target.kind != u"conversation")
			&& (target.offerBlockUserId != controller->session().userId());
		const auto block = offerBlock
			? box->addRow(
				Ui::object_ptr<Ui::Checkbox>(box, tr::lng_report_also_block(tr::now)),
				st::boxCheckboxPadding)
			: nullptr;
		const auto sending = std::make_shared<bool>(false);
		const auto submit = box->addButton(tr::lng_report_send(tr::now), nullptr, st::defaultRoundButton);
		submit->setDisabled(true);
		group->setChangedCallback([=](int) {
			submit->setDisabled(false);
		});
		const auto send = [=] {
			if (!group->hasValue() || *sending) {
				return;
			}
			*sending = true;
			submit->setLoading(true);
			const auto reason = reasons.value(group->value());
			const auto alsoBlock = block && block->checked();
			auto &data = controller->session().data();
			data.feed().sendReport(
				target.kind,
				target.id,
				reason,
				note->text().trimmed(),
				base::Guard(box),
				[=] {
					if (alsoBlock) {
						controller->session().data().graph().block(target.offerBlockUserId);
					}
					controller->showToast(tr::lng_report_sent(tr::now));
					box->closeBox();
				},
				[=](const Api::Error &error) {
					*sending = false;
					submit->setLoading(false);
					Support::ShowError(controller, error, tr::lng_eli_report_failed(tr::now));
				});
		};
		submit->setClickedCallback(send);
		QObject::connect(note, &Ui::InputField::submitted, box, send);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

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
	controller->session().data().feed().reportReasons(
		base::Guard(controller),
		[=](const Data::ReportReasons &all) {
			const auto &list = (target.kind == u"reply")
				? all.reply
				: (target.kind == u"account")
				? all.account
				: (target.kind == u"conversation")
				? all.conversation
				: all.post;
			ShowReportBox(controller, target, list.isEmpty() ? DefaultReasons() : list);
		},
		[=](const Api::Error &) {
			// The form still works with the reasons every server knows.
			ShowReportBox(controller, target, DefaultReasons());
		});
}

} // namespace Eli
