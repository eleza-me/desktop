// A command line harness over the non-GUI core: signs in and drives the same
// Main::Account / Main::Session / Data:: stores the application uses.
//
//   eleza_cli --api=http://127.0.0.1:8000 --workdir=/tmp/x login +989121234567 123456
//   eleza_cli --api=... --workdir=/tmp/x inbox
//   eleza_cli --api=... --workdir=/tmp/x history c1
//   eleza_cli --api=... --workdir=/tmp/x send c1 "hello"
//   eleza_cli --api=... --workdir=/tmp/x feed
//
// More: me, logout, config, notifications, topics, sendfile <chat> <path>,
// media <chat> <seq> [variant], avatar <user>, watch [seconds], profile <handle>.
// Options: --password=<two-step password>  --no-realtime  --lang=fa|en  --verbose
#include "api/api_auth.h"
#include "api/api_client.h"
#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_media.h"
#include "data/data_messages.h"
#include "data/data_notifications.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_sync.h"
#include "data/data_users.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_database.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

namespace {

QTextStream &Out() {
	static auto stream = QTextStream(stdout);
	return stream;
}

[[nodiscard]] QString StateName(Rt::State state) {
	switch (state) {
	case Rt::State::Stopped: return u"stopped"_s;
	case Rt::State::WaitingForNetwork: return u"waiting for network"_s;
	case Rt::State::Connecting: return u"connecting"_s;
	case Rt::State::Updating: return u"updating"_s;
	case Rt::State::Connected: return u"connected"_s;
	}
	return QString();
}

[[nodiscard]] QString Time(qint64 ms) {
	return ms
		? QDateTime::fromMSecsSinceEpoch(ms).toString(u"MM-dd HH:mm"_s)
		: u"           "_s;
}

[[nodiscard]] QString StatusName(const Data::Message &message) {
	if (!message.mine) {
		return u"in"_s;
	}
	switch (message.status) {
	case Data::MessageStatus::Queued: return u"queued"_s;
	case Data::MessageStatus::Sending: return u"sending"_s;
	case Data::MessageStatus::Sent: return u"sent"_s;
	case Data::MessageStatus::Read: return u"read"_s;
	case Data::MessageStatus::Failed: return u"FAILED"_s;
	}
	return QString();
}

[[nodiscard]] QString Preview(const Data::Message &message) {
	auto text = message.text;
	text.replace(u'\n', u' ');
	const auto kind = message.kindName.isEmpty() ? u"text"_s : message.kindName;
	if (message.kind == Data::MessageKind::Text) {
		return text;
	}
	auto result = u"["_s + kind;
	if (message.media) {
		if (message.media->width) {
			result += u" %1x%2"_s.arg(message.media->width).arg(message.media->height);
		}
		if (message.media->size) {
			result += u" %1 bytes"_s.arg(message.media->size);
		}
	}
	result += u']';
	return text.isEmpty() ? result : (result + u' ' + text);
}

class Cli final : public QObject {
public:
	Cli(const QStringList &arguments) {
		auto config = Main::Account::Config();
		config.workdir = QDir::tempPath() + u"/eleza-cli"_s;
		config.apiBase = qEnvironmentVariable("ELEZA_API", u"https://api.eleza.me"_s);
		for (const auto &argument : arguments.mid(1)) {
			if (argument.startsWith(u"--api=")) {
				config.apiBase = argument.mid(6);
			} else if (argument.startsWith(u"--workdir=")) {
				config.workdir = argument.mid(10);
			} else if (argument.startsWith(u"--password=")) {
				_password = argument.mid(11);
			} else if (argument.startsWith(u"--lang=")) {
				config.language = argument.mid(7);
			} else if (argument == u"--no-realtime") {
				config.realtime = false;
			} else if (argument == u"--verbose") {
				_verbose = true;
			} else {
				_command.push_back(argument);
			}
		}
		if (_verbose) {
			QLoggingCategory::setFilterRules(u"eleza.*=true"_s);
		}
		_account = std::make_unique<Main::Account>(config);
		connect(&_account->api(), &Api::Client::requestFailed, this, [this](
				const QString &method,
				const QString &path,
				const Api::Error &error) {
			if (_verbose) {
				Out() << "  ! " << method << ' ' << path << " -> " << error.describe() << '\n';
			}
		});
		connect(_account.get(), &Main::Account::sessionEnded, this, [](Main::SessionEndReason reason) {
			Out() << "session ended (reason " << int(reason) << ")\n";
		});
	}

	void run() {
		if (_command.isEmpty() || _command.contains(u"--help"_s) || _command.front() == u"help") {
			usage();
			finish(_command.isEmpty() ? 2 : 0);
			return;
		}
		const auto name = _command.front();
		_account->start();
		if (name == u"login") {
			login();
		} else if (name == u"config") {
			config();
		} else if (!_account->session()) {
			Out() << "not signed in: run `login <phone> <code>` first\n";
			finish(1);
		} else {
			withSession(name);
		}
	}

private:
	[[nodiscard]] QString arg(int index) const {
		return _command.value(index);
	}

	void usage() {
		Out() << "usage: eleza_cli [--api=URL] [--workdir=DIR] [--password=PW] [--lang=fa|en]\n"
			"                 [--no-realtime] [--verbose] <command>\n"
			"  login <phone> <code> | logout | me | config\n"
			"  inbox | history <chat> [limit] | send <chat> <text> | sendfile <chat> <path>\n"
			"  feed [for-you|following] | notifications | topics | profile <handle>\n"
			"  media <chat> <seq> [variant] | avatar <user> | watch [seconds]\n";
	}

	void finish(int code) {
		Out().flush();
		// Let queued work (database writes, deleteLater) run, then leave.
		QTimer::singleShot(0, this, [=, this] {
			_account = nullptr;
			QCoreApplication::exit(code);
		});
	}

	[[nodiscard]] Fn<void(const Api::Error &)> failWith(const QString &what) {
		return [=, this](const Api::Error &error) {
			Out() << what << " failed: " << error.describe() << '\n';
			finish(1);
		};
	}

	void login() {
		const auto phone = arg(1);
		const auto code = arg(2);
		if (phone.isEmpty() || code.isEmpty()) {
			usage();
			finish(2);
			return;
		}
		auto &auth = _account->auth();
		Out() << "device " << _account->deviceId() << " key " << Crypto::DeviceKey::Type()
			<< ' ' << _account->deviceKey().publicKeyBase64() << '\n';
		auth.requestCode(phone, this, [=, this, &auth](const Api::OtpChallenge &challenge) {
			Out() << "challenge " << challenge.challengeId << " (" << challenge.codeLength
				<< " digits, resend after " << challenge.resendAfter << " s)\n";
			auth.verifyCode(challenge.challengeId, code, false, this, [=, this](const Api::VerifyOutcome &outcome) {
				verified(outcome);
			}, failWith(u"verify"_s));
		}, failWith(u"request code"_s));
	}

	void verified(const Api::VerifyOutcome &outcome) {
		if (outcome.kind == Api::VerifyOutcome::Kind::TwoStep) {
			Out() << "two-step password required (hint: " << outcome.form.hint
				<< ", recovery: " << outcome.form.recoveryEmail << ")\n";
			if (_password.isEmpty()) {
				Out() << "pass --password=<password>\n";
				finish(1);
				return;
			}
			_account->auth().signInWithPassword(outcome.form, _password, this, [=, this](const Api::VerifyOutcome &next) {
				verified(next);
			}, failWith(u"two-step"_s));
			return;
		}
		Out() << "signed in as " << outcome.userId
			<< (outcome.needsProfile ? " (profile incomplete)" : "")
			<< "; session " << _account->sessionId() << '\n';
		finish(0);
	}

	void config() {
		_account->clientConfig().load(this, [=, this](bool fresh) {
			const auto &config = _account->clientConfig();
			Out() << "client-config (" << (fresh ? "fresh" : "cached / defaults") << ")\n"
				<< "  min desktop version   " << config.minClientVersion() << '\n'
				<< "  realtime v2           " << (config.realtimeV2Enabled() ? "on" : "off") << '\n'
				<< "  max message length    " << config.maxMessageLength() << '\n'
				<< "  conversation topic    " << config.conversationTopicMaxChars() << " chars, durations "
				<< config.conversationDurations().join(u", ") << " (default " << config.conversationDefaultDuration() << ")\n"
				<< "  categories            ";
			for (const auto &category : config.conversationCategories()) {
				Out() << category.slug << ' ';
			}
			Out() << '\n';
			finish(0);
		});
	}

	void withSession(const QString &name) {
		const auto session = _account->session();
		connect(session, &Main::Session::connectionStateChanged, this, [=, this](Rt::State state) {
			if (_verbose || name == u"watch") {
				Out() << "  realtime: " << StateName(state) << '\n';
				Out().flush();
			}
		});
		const auto go = [=, this] {
			dispatch(name);
		};
		if (session->isReady()) {
			go();
		} else {
			connect(session, &Main::Session::ready, this, go);
		}
	}

	void dispatch(const QString &name) {
		const auto session = _account->session();
		auto &data = session->data();
		if (name == u"logout") {
			_account->logout(this, [=, this] {
				Out() << "signed out\n";
				finish(0);
			});
		} else if (name == u"me") {
			data.users().refreshMe(this, [=, this](const Data::Profile &me) {
				Out() << me.id << "  @" << me.handle << "  " << me.displayName
					<< (me.verified ? " [verified]" : "") << '\n'
					<< "  bio: " << me.bio << '\n'
					<< "  followers " << me.followers << ", following " << me.following
					<< ", posts " << me.posts << '\n';
				finish(0);
			}, failWith(u"me"_s));
		} else if (name == u"inbox") {
			inbox();
		} else if (name == u"history") {
			history(arg(1), arg(2).toInt());
		} else if (name == u"send") {
			send();
		} else if (name == u"sendfile") {
			sendFile();
		} else if (name == u"feed") {
			feed();
		} else if (name == u"notifications") {
			notifications();
		} else if (name == u"topics") {
			topics();
		} else if (name == u"profile") {
			data.graph().profileByHandle(arg(1), this, [=, this](const Data::Profile &profile) {
				Out() << profile.id << "  @" << profile.handle << "  " << profile.displayName << '\n'
					<< "  " << profile.bio << '\n'
					<< "  followers " << profile.followers << ", follow state " << profile.followState
					<< (profile.followsYou ? ", follows you" : "") << '\n';
				finish(0);
			}, failWith(u"profile"_s));
		} else if (name == u"media") {
			media(Data::MediaRef::Attachment(arg(1), arg(2).toLongLong()), arg(3));
		} else if (name == u"avatar") {
			// The mock serves an avatar for any id.
			const auto card = data.users().peek(arg(1));
			media(Data::MediaRef::Avatar(arg(1), card.avatarId.isEmpty() ? u"current"_s : card.avatarId), u"thumb"_s);
		} else if (name == u"watch") {
			watch();
		} else {
			usage();
			finish(2);
		}
	}

	void printInbox() {
		auto &chats = _account->session()->data().chats();
		const auto section = [&](const char *title, Data::Chats::Section which) {
			const auto rows = chats.list(which);
			if (rows.isEmpty()) {
				return;
			}
			Out() << title << " (" << rows.size() << ")\n";
			for (const auto &chat : rows) {
				auto title = chat.saved
					? u"<saved messages>"_s
					: chat.title.isEmpty()
					? (chat.peerDeleted ? u"<deleted account>"_s : u"…"_s)
					: chat.title;
				auto flags = QString();
				if (chat.pinned) flags += u" pinned"_s;
				if (chat.muted) flags += u" muted"_s;
				if (chat.isBot) flags += u" bot"_s;
				if (chat.verified) flags += u" verified"_s;
				if (chat.anonymous) flags += u" anonymous"_s;
				Out() << "  " << chat.id.leftJustified(12) << ' ' << Time(chat.lastActivityAt)
					<< "  " << title;
				if (chat.unread) {
					Out() << "  (" << chat.unread << " unread)";
				}
				Out() << (flags.isEmpty() ? QString() : (u"  ["_s + flags.trimmed() + u']')) << '\n';
				if (chat.draft) {
					Out() << "      draft: " << chat.draft->text << '\n';
				} else if (chat.lastMessage) {
					Out() << "      " << (chat.lastMine ? "me: " : "") << Preview(*chat.lastMessage) << '\n';
				}
			}
		};
		section("messages", Data::Chats::Section::Messages);
		section("conversations", Data::Chats::Section::Conversations);
		section("requests", Data::Chats::Section::Requests);
		section("archived", Data::Chats::Section::Archived);
		Out() << "unread chats: " << chats.unreadChats() << '\n';
	}

	void inbox() {
		auto &data = _account->session()->data();
		const auto cached = data.chats().count(Data::Chats::Section::All);
		Out() << "cache: " << cached << " chats; loading the inbox...\n";
		data.chats().loadInbox(this, [=, this] {
			Out() << "transport: "
				<< (_account->session()->rt().isConnected() ? "realtime" : "HTTP (no realtime connection)")
				<< '\n';
			printInbox();
			finish(0);
		}, failWith(u"inbox"_s));
	}

	void printHistory(const QString &chat, int limit) {
		auto &data = _account->session()->data();
		const auto &history = data.messages().history(chat);
		const auto from = (limit > 0) ? std::max<qsizetype>(0, history.items.size() - limit) : 0;
		for (auto i = from; i != history.items.size(); ++i) {
			const auto &message = history.items[i];
			const auto sender = message.mine
				? u"me"_s
				: data.users().peek(message.sender).displayName;
			Out() << "  " << (message.seq ? QString::number(message.seq) : u"-"_s).rightJustified(4)
				<< ' ' << Time(message.createdAt) << "  "
				<< (sender.isEmpty() ? message.sender : sender).leftJustified(12)
				<< ' ' << StatusName(message).leftJustified(7) << ' ' << Preview(message);
			if (message.replyToSeq) {
				Out() << "  (reply to " << message.replyToSeq << ")";
			}
			if (!message.reactions.isEmpty()) {
				Out() << "  {";
				for (auto r = message.reactions.cbegin(); r != message.reactions.cend(); ++r) {
					Out() << r.key() << r.value();
				}
				Out() << '}';
			}
			if (message.edited) {
				Out() << "  (edited)";
			}
			Out() << '\n';
		}
		Out() << "  -- " << history.items.size() << " messages"
			<< (history.hasMoreBefore ? ", more before" : ", start of chat") << '\n';
	}

	void history(const QString &chat, int limit) {
		if (chat.isEmpty()) {
			usage();
			finish(2);
			return;
		}
		auto &messages = _account->session()->data().messages();
		messages.loadCached(chat, this, [=, this, &messages](int cached) {
			Out() << "cache: " << cached << " messages; loading the newest page...\n";
			messages.loadLatest(chat, this, [=, this] {
				printHistory(chat, limit);
				finish(0);
			}, failWith(u"history"_s));
		});
	}

	void waitForOutbox(const QString &chat, const QString &id) {
		auto &data = _account->session()->data();
		if (id.isEmpty()) {
			Out() << "could not queue the message\n";
			finish(1);
			return;
		}
		Out() << "queued " << id << '\n';
		connect(&data.outbox(), &Data::Outbox::sent, this, [=, this](
				const QString &sentChat,
				const QString &sentId,
				qint64 seq) {
			if (sentId != id) {
				return;
			}
			Out() << "sent to " << sentChat << " as seq " << seq << " over "
				<< (_account->session()->rt().isConnected() ? "realtime" : "HTTP") << '\n';
			printHistory(chat, 4);
			finish(0);
		});
		connect(&data.outbox(), &Data::Outbox::failed, this, [=, this](
				const QString &,
				const QString &failedId,
				const Api::Error &error) {
			if (failedId == id) {
				Out() << "send failed: " << error.describe() << '\n';
				finish(1);
			}
		});
	}

	void send() {
		const auto chat = arg(1);
		const auto text = _command.mid(2).join(u' ');
		if (chat.isEmpty() || text.isEmpty()) {
			usage();
			finish(2);
			return;
		}
		auto &data = _account->session()->data();
		// Open the timeline first so the echo lands in a loaded window.
		data.messages().loadLatest(chat, this, [=, this, &data] {
			waitForOutbox(chat, data.outbox().sendText(chat, text));
		}, failWith(u"history"_s));
	}

	void sendFile() {
		const auto chat = arg(1);
		const auto path = arg(2);
		auto &data = _account->session()->data();
		data.messages().loadLatest(chat, this, [=, this, &data] {
			connect(&data.messages(), &Data::Messages::historyChanged, this, [=, this, &data](const QString &changed) {
				if (changed != chat) {
					return;
				}
				for (const auto &message : data.messages().history(chat).items) {
					if (message.local() && message.uploadProgress >= 0. && message.uploadProgress != _lastProgress) {
						_lastProgress = message.uploadProgress;
						Out() << "  upload " << int(message.uploadProgress * 100) << "%\n";
					}
				}
			});
			waitForOutbox(chat, data.outbox().sendFile(chat, path));
		}, failWith(u"history"_s));
	}

	void feed() {
		auto &data = _account->session()->data();
		const auto tab = (arg(1) == u"following") ? Data::Feed::Tab::Following : Data::Feed::Tab::ForYou;
		data.feed().loadFeed(tab, QString(), this, [=, this, &data](const Data::FeedPage &page) {
			auto authors = QStringList();
			for (const auto &item : page.items) {
				if (const auto post = data.feed().post(item.postId)) {
					authors.push_back(post->author);
				}
			}
			data.users().ensure(authors, this, [=, this, &data] {
				Out() << "feed: " << page.items.size() << " posts"
					<< (page.nextCursor.isEmpty() ? "" : " (more)") << '\n';
				for (const auto &item : page.items) {
					const auto post = data.feed().post(item.postId);
					if (!post) {
						continue;
					}
					const auto author = data.users().peek(post->author);
					auto text = post->text;
					text.replace(u'\n', u' ');
					Out() << "  " << post->id.leftJustified(6) << ' ' << Time(post->createdAt) << "  @"
						<< author.handle.leftJustified(10) << ' ' << post->type.leftJustified(8) << ' ' << text << '\n';
					auto extra = QStringList();
					if (!item.reason.isEmpty()) {
						extra.push_back(item.reason + (item.actor.isEmpty() ? QString() : (u" by "_s + item.actor)));
					}
					if (!post->media.isEmpty()) {
						extra.push_back(u"%1 media"_s.arg(post->media.size()));
					}
					if (post->poll) {
						extra.push_back(u"poll: "_s + post->poll->options.join(u" / "));
					}
					if (post->quote) {
						extra.push_back(u"quotes "_s + post->quote->id);
					}
					if (post->conversation) {
						extra.push_back(u"invite to «"_s + post->conversation->topic + u'»');
					}
					if (!post->threadParts.isEmpty()) {
						extra.push_back(u"%1 thread parts"_s.arg(post->threadParts.size()));
					}
					Out() << "         likes " << post->counts.likes << ", replies " << post->counts.replies
						<< ", reposts " << post->counts.reposts << ", views " << post->counts.views
						<< (post->my.reaction.isEmpty() ? QString() : (u", my reaction "_s + post->my.reaction))
						<< (extra.isEmpty() ? QString() : (u"  ("_s + extra.join(u"; ") + u')')) << '\n';
				}
				finish(0);
			});
		}, failWith(u"feed"_s));
	}

	void notifications() {
		auto &data = _account->session()->data();
		data.notifications().refreshUnread(this, [=, this, &data] {
			Out() << "unread: " << data.notifications().unread() << "  follow requests: "
				<< data.notifications().kind(u"follow_request"_s) << '\n';
			data.notifications().load(QString(), 0, this, [=, this, &data](const QVector<Data::NotificationEntry> &list) {
				for (const auto &entry : list) {
					auto actors = QStringList();
					for (const auto &actor : entry.actors) {
						actors.push_back(data.users().peek(actor).displayName);
					}
					Out() << "  " << entry.id.leftJustified(4) << Time(entry.updatedAt) << "  "
						<< entry.kind.leftJustified(20) << actors.join(u", ")
						<< (entry.read ? "" : "  *") << '\n';
				}
				finish(0);
			}, failWith(u"notifications"_s));
		});
	}

	void topics() {
		auto &data = _account->session()->data();
		data.conversations().loadOpen(QString(), QString(), this, [=, this, &data](const Data::OpenConversationsPage &page) {
			Out() << "open conversations: " << page.conversations.size() << '\n';
			for (const auto &item : page.conversations) {
				Out() << "  " << item.id.leftJustified(8) << data.conversations().categoryTitle(item.category).leftJustified(14)
					<< (item.speakerMasked ? (u"["_s + item.maskName + u"] "_s) : QString()) << item.topic << '\n';
			}
			finish(0);
		}, failWith(u"topics"_s));
	}

	void media(const Data::MediaRef &ref, const QString &variantName) {
		auto variant = Data::MediaVariant::Thumb;
		for (const auto candidate : { Data::MediaVariant::Feed, Data::MediaVariant::Full, Data::MediaVariant::File }) {
			if (Data::MediaVariantName(candidate) == variantName) {
				variant = candidate;
			}
		}
		auto &media = _account->session()->data().media();
		_handle = media.request(ref, variant);
		const auto report = [=, this] {
			if (_handle->isReady()) {
				Out() << "media ready: " << _handle->image().width() << 'x' << _handle->image().height()
					<< " image, file " << _handle->filePath() << '\n';
				finish(0);
			} else {
				Out() << "media failed: " << _handle->error().describe() << '\n';
				finish(1);
			}
		};
		if (_handle->state() != Data::MediaHandle::State::Loading) {
			report();
			return;
		}
		connect(_handle.get(), &Data::MediaHandle::ready, this, report);
		connect(_handle.get(), &Data::MediaHandle::failed, this, report);
	}

	void watch() {
		const auto session = _account->session();
		const auto seconds = std::max(1, arg(1).isEmpty() ? 10 : arg(1).toInt());
		Out() << "realtime: " << StateName(session->rt().state()) << " (watching " << seconds << " s)\n";
		connect(&session->rt(), &Rt::Client::frameReceived, this, [](const Rt::Frame &frame) {
			Out() << "  frame type " << int(frame.type) << " pts " << frame.pts << " kind " << frame.kind
				<< " chat " << frame.chat << '\n';
			Out().flush();
		});
		connect(&session->data().sync(), &Data::Sync::messageReceived, this, [](const Data::Message &message) {
			Out() << "  message in " << message.chat << " #" << message.seq << ": " << Preview(message) << '\n';
			Out().flush();
		});
		QTimer::singleShot(seconds * 1000, this, [=, this] {
			Out() << "pts " << session->data().sync().pts().value_or(0) << ", state "
				<< StateName(session->rt().state()) << '\n';
			finish(0);
		});
	}

	std::unique_ptr<Main::Account> _account;
	Data::MediaHandlePtr _handle;
	QStringList _command;
	QString _password;
	double _lastProgress = -1.;
	bool _verbose = false;

};

} // namespace

int main(int argc, char *argv[]) {
	auto application = QCoreApplication(argc, argv);
	QCoreApplication::setApplicationName(u"eleza_cli"_s);
	auto cli = Cli(application.arguments());
	QTimer::singleShot(0, &cli, [&] { cli.run(); });
	// A safety net for scripts: never hang for ever.
	QTimer::singleShot(120000, &application, [] {
		Out() << "timed out\n";
		Out().flush();
		QCoreApplication::exit(3);
	});
	return application.exec();
}
