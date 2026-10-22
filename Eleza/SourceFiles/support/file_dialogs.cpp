#include "support/file_dialogs.h"

#include "base/platform_file.h"
#include "data/data_image_decode.h"
#include "core/core_application.h"
#include "lang_auto.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QMimeDatabase>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QThreadPool>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QFileDialog>

namespace Support {
namespace {

const auto kImageExtensions = QStringList{
	QStringLiteral("png"),
	QStringLiteral("jpg"),
	QStringLiteral("jpeg"),
	QStringLiteral("webp"),
	QStringLiteral("gif"),
};
const auto kVideoExtensions = QStringList{
	QStringLiteral("mp4"),
	QStringLiteral("mov"),
	QStringLiteral("m4v"),
	QStringLiteral("webm"),
	QStringLiteral("mkv"),
	QStringLiteral("avi"),
};
const auto kAudioExtensions = QStringList{
	QStringLiteral("mp3"),
	QStringLiteral("m4a"),
	QStringLiteral("aac"),
	QStringLiteral("ogg"),
	QStringLiteral("oga"),
	QStringLiteral("opus"),
	QStringLiteral("wav"),
	QStringLiteral("flac"),
};

[[nodiscard]] QString Patterns(const QStringList &extensions) {
	auto parts = QStringList();
	for (const auto &extension : extensions) {
		parts.push_back(QStringLiteral("*.") + extension);
	}
	return parts.join(QLatin1Char(' '));
}

[[nodiscard]] bool HasExtension(const QString &path, const QStringList &list) {
	return list.contains(QFileInfo(path).suffix().toLower());
}

[[nodiscard]] QString StartDirectory() {
	const auto saved = Core::App().settings().lastDialogDirectory();
	if (!saved.isEmpty() && QDir(saved).exists()) {
		return saved;
	}
	const auto documents = QStandardPaths::writableLocation(
		QStandardPaths::DocumentsLocation);
	return documents.isEmpty() ? QDir::homePath() : documents;
}

void RememberDirectory(const QString &filePath) {
	const auto directory = QFileInfo(filePath).absolutePath();
	if (!directory.isEmpty()) {
		Core::App().settings().setLastDialogDirectory(directory);
	}
}

[[nodiscard]] QStringList NameFilters(PickKind kind) {
	const auto all = tr::lng_support_files_all(tr::now) + QStringLiteral(" (*)");
	switch (kind) {
	case PickKind::Images:
		return {
			tr::lng_support_files_images(tr::now)
				+ QStringLiteral(" (") + Patterns(kImageExtensions) + QLatin1Char(')'),
			all,
		};
	case PickKind::Videos:
		return {
			tr::lng_support_files_videos(tr::now)
				+ QStringLiteral(" (") + Patterns(kVideoExtensions) + QLatin1Char(')'),
			all,
		};
	case PickKind::Media:
		return {
			tr::lng_support_files_media(tr::now)
				+ QStringLiteral(" (")
				+ Patterns(kImageExtensions + kVideoExtensions)
				+ QLatin1Char(')'),
			all,
		};
	case PickKind::Audio:
		return {
			tr::lng_support_files_audio(tr::now)
				+ QStringLiteral(" (") + Patterns(kAudioExtensions) + QLatin1Char(')'),
			all,
		};
	case PickKind::Any:
		break;
	}
	return { all };
}

[[nodiscard]] QString DefaultTitle(const PickArgs &args) {
	if (!args.title.isEmpty()) {
		return args.title;
	}
	switch (args.kind) {
	case PickKind::Images: return tr::lng_support_pick_image(tr::now);
	case PickKind::Videos:
	case PickKind::Media: return tr::lng_chat_photo_video(tr::now);
	default: break;
	}
	return tr::lng_support_pick_file(tr::now);
}

[[nodiscard]] QStringList ExistingFiles(const QStringList &paths) {
	auto result = QStringList();
	for (const auto &path : paths) {
		const auto info = QFileInfo(path);
		if (info.isFile() && info.isReadable()) {
			result.push_back(info.absoluteFilePath());
		}
	}
	return result;
}

// Runs `work` on a pool thread and `done(result)` back on the GUI thread
// while `guard` is alive.
template <typename Work, typename Done>
void InBackground(QObject *guard, Work work, Done done) {
	const auto weak = QPointer<QObject>(guard);
	const auto bound = (guard != nullptr);
	QThreadPool::globalInstance()->start([=]() mutable {
		auto result = work();
		QMetaObject::invokeMethod(qApp, [=, result = std::move(result)] {
			if (!bound || weak) {
				done(result);
			}
		}, Qt::QueuedConnection);
	});
}

} // namespace

bool IsImageFile(const QString &path) {
	// By the name and by the content: what the decoder would refuse is not
	// treated as a picture anywhere (it is attached as a file).
	return HasExtension(path, kImageExtensions)
		&& Data::IsSupportedImageFile(path);
}

bool IsVideoFile(const QString &path) {
	return HasExtension(path, kVideoExtensions);
}

bool IsAudioFile(const QString &path) {
	return HasExtension(path, kAudioExtensions);
}

void PickFiles(
		QWidget *parent,
		PickArgs args,
		std::function<void(const QStringList &paths)> done) {
#ifdef ELEZA_DEBUG_TOOLS
	if (const auto debug = qEnvironmentVariable("ELEZA_DEBUG_PICK");
		!debug.isEmpty()) {
		auto paths = ExistingFiles(debug.split(QLatin1Char(';'), Qt::SkipEmptyParts));
		if (!args.multiple && paths.size() > 1) {
			paths = paths.mid(0, 1);
		}
		const auto weak = QPointer<QWidget>(parent);
		QMetaObject::invokeMethod(qApp, [=] {
			if (weak && done && !paths.isEmpty()) {
				done(paths);
			}
		}, Qt::QueuedConnection);
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	const auto dialog = new QFileDialog(
		parent ? parent->window() : nullptr,
		DefaultTitle(args),
		StartDirectory());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setAcceptMode(QFileDialog::AcceptOpen);
	dialog->setFileMode(args.multiple
		? QFileDialog::ExistingFiles
		: QFileDialog::ExistingFile);
	dialog->setNameFilters(NameFilters(args.kind));
	dialog->setWindowModality(Qt::WindowModal);
	const auto weak = QPointer<QWidget>(parent);
	QObject::connect(dialog, &QFileDialog::filesSelected, dialog, [=](
			const QStringList &selected) {
		const auto paths = ExistingFiles(selected);
		if (paths.isEmpty()) {
			return;
		}
		RememberDirectory(paths.front());
		if (weak && done) {
			done(paths);
		}
	});
	dialog->open();
}

void PickImage(QWidget *parent, std::function<void(const QString &path)> done) {
	auto args = PickArgs();
	args.kind = PickKind::Images;
	PickFiles(parent, args, [done = std::move(done)](const QStringList &paths) {
		if (done && !paths.isEmpty()) {
			done(paths.front());
		}
	});
}

void AskSavePath(
		QWidget *parent,
		const QString &suggestedName,
		std::function<void(const QString &path)> done,
		const QString &title) {
#ifdef ELEZA_DEBUG_TOOLS
	if (const auto debug = qEnvironmentVariable("ELEZA_DEBUG_SAVE");
		!debug.isEmpty()) {
		const auto weak = QPointer<QWidget>(parent);
		QMetaObject::invokeMethod(qApp, [=] {
			if (weak && done) {
				done(debug);
			}
		}, Qt::QueuedConnection);
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	// A name from the network must stay a plain file name, valid on every
	// file system (Windows: no <>:"|?*, no device names, no trailing dots).
	const auto name = base::platform::SafeFileName(
		QFileInfo(suggestedName).fileName(),
		QStringLiteral("file"));
	const auto dialog = new QFileDialog(
		parent ? parent->window() : nullptr,
		title.isEmpty() ? tr::lng_support_save_as(tr::now) : title,
		QDir(StartDirectory()).filePath(name));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setAcceptMode(QFileDialog::AcceptSave);
	dialog->setFileMode(QFileDialog::AnyFile);
	dialog->setWindowModality(Qt::WindowModal);
	const auto suffix = QFileInfo(name).suffix();
	if (!suffix.isEmpty()) {
		dialog->setDefaultSuffix(suffix);
	}
	const auto weak = QPointer<QWidget>(parent);
	QObject::connect(dialog, &QFileDialog::fileSelected, dialog, [=](
			const QString &path) {
		if (path.isEmpty()) {
			return;
		}
		RememberDirectory(path);
		if (weak && done) {
			done(path);
		}
	});
	dialog->open();
}

void SaveFileAs(
		QWidget *parent,
		const QString &sourcePath,
		const QString &suggestedName,
		std::function<void(bool saved)> done) {
	const auto weak = QPointer<QWidget>(parent);
	AskSavePath(parent, suggestedName, [=](const QString &path) {
		InBackground(weak.data(), [=] {
			if (QFileInfo(path) == QFileInfo(sourcePath)) {
				return true;
			}
			QFile::remove(path);
			return QFile::copy(sourcePath, path);
		}, [=](bool saved) {
			if (done) {
				done(saved);
			}
		});
	});
}

void SaveImageAs(
		QWidget *parent,
		const QImage &image,
		const QString &suggestedName,
		std::function<void(bool saved)> done) {
	const auto weak = QPointer<QWidget>(parent);
	AskSavePath(parent, suggestedName, [=](const QString &path) {
		InBackground(weak.data(), [=] {
			return image.save(path);
		}, [=](bool saved) {
			if (done) {
				done(saved);
			}
		});
	});
}

bool CanAcceptDrop(const QMimeData *data) {
	if (!data) {
		return false;
	} else if (data->hasImage()) {
		return true;
	}
	const auto urls = data->urls();
	for (const auto &url : urls) {
		if (url.isLocalFile()) {
			return true;
		}
	}
	return false;
}

DroppedData ReadMimeData(const QMimeData *data) {
	auto result = DroppedData();
	if (!data) {
		return result;
	}
	auto paths = QStringList();
	const auto urls = data->urls();
	for (const auto &url : urls) {
		if (url.isLocalFile()) {
			paths.push_back(url.toLocalFile());
		}
	}
	result.files = ExistingFiles(paths);
	if (result.files.isEmpty() && data->hasImage()) {
		result.image = qvariant_cast<QImage>(data->imageData());
	}
	if (result.files.isEmpty() && result.image.isNull() && data->hasText()) {
		result.text = data->text();
	}
	return result;
}

QImage ClipboardImage() {
	const auto data = QGuiApplication::clipboard()->mimeData();
	return (data && data->hasImage())
		? qvariant_cast<QImage>(data->imageData())
		: QImage();
}

void SaveImageToTemp(
		const QImage &image,
		QObject *guard,
		std::function<void(const QString &path)> done) {
	if (image.isNull()) {
		if (done) {
			done(QString());
		}
		return;
	}
	const auto directory = QDir(base::platform::PrivateTempDirectory()).filePath(
		QStringLiteral("eleza-paste"));
	InBackground(guard, [=] {
		QDir().mkpath(directory);
		const auto path = QDir(directory).filePath(
			QUuid::createUuid().toString(QUuid::WithoutBraces)
				+ QStringLiteral(".png"));
		return image.save(path, "PNG") ? path : QString();
	}, [=](const QString &path) {
		if (done) {
			done(path);
		}
	});
}

void SaveFileToTemp(
		const QString &sourcePath,
		const QByteArray &bytes,
		const QString &fallbackSuffix,
		QObject *guard,
		std::function<void(const QString &path)> done) {
	if (sourcePath.isEmpty() && bytes.isEmpty()) {
		if (done) {
			done(QString());
		}
		return;
	}
	const auto directory = QDir(base::platform::PrivateTempDirectory()).filePath(
		QStringLiteral("eleza-share"));
	InBackground(guard, [=] {
		auto head = bytes.left(512);
		if (head.isEmpty()) {
			auto file = QFile(sourcePath);
			if (!file.open(QIODevice::ReadOnly)) {
				return QString();
			}
			head = file.read(512);
		}
		const auto mime = QMimeDatabase().mimeTypeForData(head);
		const auto known = mime.name().startsWith(u"image/")
			|| mime.name().startsWith(u"video/");
		auto suffix = known ? mime.preferredSuffix() : QString();
		if (suffix.isEmpty()) {
			suffix = fallbackSuffix;
		}
		if (suffix.startsWith(QLatin1Char('.'))) {
			suffix = suffix.mid(1);
		}
		QDir().mkpath(directory);
		const auto path = QDir(directory).filePath(
			QUuid::createUuid().toString(QUuid::WithoutBraces)
				+ QLatin1Char('.')
				+ suffix);
		if (!bytes.isEmpty()) {
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(bytes) != bytes.size()) {
				file.remove();
				return QString();
			}
			return path;
		}
		return QFile::copy(sourcePath, path) ? path : QString();
	}, [=](const QString &path) {
		if (done) {
			done(path);
		}
	});
}

} // namespace Support
