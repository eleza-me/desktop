// Widget-free rules of chat albums (compiled into the unit tests as is):
// which messages of a timeline make one album bubble, what that bubble says
// (one caption, one time, one status), the mosaic its tiles are laid out in,
// and how a set of picked files is sent (which of them share an album, where
// the caption goes).
//
// The mosaic is the one the Android and iOS apps draw (the same ratios give
// the same frames; tests/app/data/album_layout_vectors.json are the shared
// vectors).
#pragma once

#include "data/data_types.h"

#include <vector>

namespace History {

// ---- the mosaic

// Frames are in an abstract box kAlbumMaxWidth units wide, without the gaps
// (the view insets each tile by half the gap on its inner sides).
inline constexpr auto kAlbumMaxWidth = 800.;
inline constexpr auto kAlbumMaxHeight = 814.;
inline constexpr auto kAlbumMinWidth = 120.;
inline constexpr auto kAlbumMinHeight = 120.;

// The outer sides of the album a tile touches (its rounded corners).
enum AlbumSide {
	kAlbumSideLeft = 1,
	kAlbumSideTop = 2,
	kAlbumSideRight = 4,
	kAlbumSideBottom = 8,
};

struct AlbumFrame {
	double x = 0.;
	double y = 0.;
	double w = 0.;
	double h = 0.;
	int sides = 0;
};

// `ratios`: width / height of the items in order; an unknown size (0, NaN,
// infinite) counts as a square. One frame per item.
[[nodiscard]] std::vector<AlbumFrame> AlbumLayout(const std::vector<double> &ratios);
// The album's height in the abstract box: max(y + h).
[[nodiscard]] double AlbumHeight(const std::vector<AlbumFrame> &frames);

// ---- the timeline

// Kinds that may share one album bubble: photos with videos, files with
// files, songs with songs.
enum class AlbumFamily {
	None,
	Visual,
	File,
	Audio,
};
[[nodiscard]] AlbumFamily AlbumFamilyOf(Data::MessageKind kind);

// What a message groups by: its album id, else (sent before album ids
// existed) the shared prefix of an album's client message ids («a1b» mark).
// Empty: not an item of an album.
[[nodiscard]] QString AlbumKey(const Data::Message &message);

struct AlbumRun {
	int from = 0;
	int count = 0;
};
// Runs of two or more consecutive photos / videos of one album: the same
// key, the same side and, in a group, the same sender; at most `maxItems`
// per bubble (a longer run continues in the next one). Sent messages and my
// queued ones alike: the album is one bubble from the moment it is sent.
[[nodiscard]] std::vector<AlbumRun> GroupAlbums(
	const QVector<Data::Message> &items,
	bool group,
	int maxItems);

// The item whose text is the album's caption (the first that has one), or
// -1 when no item has a caption.
[[nodiscard]] int AlbumCaptionIndex(const QVector<Data::Message> &items);

// The message an album bubble stands for: the first item (its key, seq,
// reply, reactions) with the album's caption, the newest item's time and,
// for mine, the least-sent item's status (failed wins, then sending).
[[nodiscard]] Data::Message AlbumHeadMessage(const QVector<Data::Message> &items);

// ---- sending

struct AlbumSendPlan {
	// One entry per file: the index of its album (files with the same index
	// share an album id), or -1 for a file sent on its own.
	std::vector<int> albums;
	int albumCount = 0;
	// The caption goes on the first file; otherwise it is sent as a text
	// message of its own after the files (files keep their names as text).
	bool captionOnFirst = false;
};
// `families`: how each picked file will be sent (Visual: a photo / video,
// File, Audio). `grouped`: several files are sent as albums (consecutive
// files of one family, at most `maxItems` in each).
[[nodiscard]] AlbumSendPlan PlanAlbumSend(
	const std::vector<AlbumFamily> &families,
	bool grouped,
	int maxItems);

// Messages forwarded together: the items of one album (consecutive, the
// same album, one family) stay one album at the destination, under a new
// id. One entry per message: its new album's index, or -1.
[[nodiscard]] std::vector<int> ForwardAlbumGroups(
	const QVector<Data::Message> &messages,
	int maxItems);

} // namespace History
