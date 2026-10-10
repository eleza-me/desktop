// Choosing and saving files, the clipboard and drag-and-drop payloads.
//
//   Support::PickFiles(this, { .kind = Support::PickKind::Media, .multiple = true },
//       [=](const QStringList &paths) { attach(paths); });
//   Support::PickImage(this, [=](const QString &path) { ... });
//   Support::SaveFileAs(this, cachedPath, u"photo.jpg"_s, [=](bool saved) { ... });
//
//   // paste / drop in a composer
//   if (const auto data = Support::ReadMimeData(QGuiApplication::clipboard()->mimeData()); !data.empty()) ...
//   void dragEnterEvent(QDragEnterEvent *e) { if (Support::CanAcceptDrop(e->mimeData())) e->acceptProposedAction(); }
//   void dropEvent(QDropEvent *e) { const auto data = Support::ReadMimeData(e->mimeData()); ... }
//   Support::SaveImageToTemp(data.image, this, [=](const QString &path) { attach({ path }); });
//
// The dialogs are window-modal and never block the caller: the answer arrives in
// the callback (not at all when the dialog is cancelled or the guard widget
// died). The last directory is remembered in Core::Settings.
//
// Headless runs (builds with ELEZA_DEBUG_TOOLS only): the environment variable
// ELEZA_DEBUG_PICK ("path1;path2") answers every pick dialog at once,
// ELEZA_DEBUG_SAVE (a file path) every save dialog.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QImage>

#include <functional>

class QMimeData;
class QObject;
class QWidget;

namespace Support {

enum class PickKind {
	Images, // photos (png, jpg, webp, gif, bmp)
	Videos,
	Media, // images and videos
	Audio,
	Any,
};

struct PickArgs {
	PickKind kind = PickKind::Any;
	bool multiple = false;
	QString title; // default: by the kind
};

void PickFiles(
	QWidget *parent,
	PickArgs args,
	std::function<void(const QStringList &paths)> done);
void PickImage(QWidget *parent, std::function<void(const QString &path)> done);

// Asks where to save; `done` gets the chosen path.
void AskSavePath(
	QWidget *parent,
	const QString &suggestedName,
	std::function<void(const QString &path)> done,
	const QString &title = QString());
// Asks where, then copies `sourcePath` there on a worker thread.
void SaveFileAs(
	QWidget *parent,
	const QString &sourcePath,
	const QString &suggestedName,
	std::function<void(bool saved)> done = nullptr);
// The same for an image that only exists in memory (png or jpg by the name).
void SaveImageAs(
	QWidget *parent,
	const QImage &image,
	const QString &suggestedName,
	std::function<void(bool saved)> done = nullptr);

// A picture by its name and by its content (Data::IsSupportedImageFile: JPEG,
// PNG, GIF, WebP within the decoder's limits). Reads the file's header.
[[nodiscard]] bool IsImageFile(const QString &path);
[[nodiscard]] bool IsVideoFile(const QString &path);
[[nodiscard]] bool IsAudioFile(const QString &path);

// ---- clipboard and drag-and-drop
struct DroppedData {
	QStringList files; // local files that exist
	QImage image; // a bitmap without a file (a screenshot, "copy image")
	QString text;

	[[nodiscard]] bool hasMedia() const {
		return !files.isEmpty() || !image.isNull();
	}
	[[nodiscard]] bool empty() const {
		return files.isEmpty() && image.isNull() && text.isEmpty();
	}
};
// Local files or an image: something a composer can attach.
[[nodiscard]] bool CanAcceptDrop(const QMimeData *data);
[[nodiscard]] DroppedData ReadMimeData(const QMimeData *data);
// The image on the clipboard right now (null if there is none).
[[nodiscard]] QImage ClipboardImage();

// Writes the image as a PNG into the temporary directory on a worker thread;
// `done` gets the file path ("" on failure) while `guard` is alive.
void SaveImageToTemp(
	const QImage &image,
	QObject *guard,
	std::function<void(const QString &path)> done);

} // namespace Support
