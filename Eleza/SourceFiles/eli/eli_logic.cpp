#include "eli/eli_logic.h"

#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace Eli::Logic {
namespace {

constexpr auto kWordsPerMinute = 180;

// Index (in UTF-16 units) right after `count` code points from `from`.
[[nodiscard]] int AdvanceCodePoints(const QString &text, int from, int count) {
	auto i = from;
	while (count > 0 && i < text.size()) {
		if (text.at(i).isHighSurrogate()
			&& i + 1 < text.size()
			&& text.at(i + 1).isLowSurrogate()) {
			i += 2;
		} else {
			++i;
		}
		--count;
	}
	return i;
}

[[nodiscard]] bool IsSentenceEnd(QChar ch) {
	switch (ch.unicode()) {
	case '.':
	case '!':
	case '?':
	case 0x061F: // Arabic question mark
	case 0x06D4: // Arabic full stop
	case 0x2026: // ellipsis
		return true;
	}
	return false;
}

} // namespace

double ClampAspect(int width, int height) {
	if (width <= 0 || height <= 0) {
		return 1.;
	}
	return std::clamp(double(width) / double(height), kMinAspect, kMaxAspect);
}

MediaLayout LayoutMedia(
		const QVector<QSize> &sizes,
		int width,
		int skip,
		int maxHeight) {
	auto result = MediaLayout();
	const auto count = int(sizes.size());
	if (count <= 0 || width <= 0) {
		return result;
	}
	if (count == 1) {
		const auto aspect = ClampAspect(sizes[0].width(), sizes[0].height());
		auto height = int(std::lround(width / aspect));
		auto tileWidth = width;
		if (maxHeight > 0 && height > maxHeight) {
			height = maxHeight;
			tileWidth = std::min(width, int(std::lround(height * aspect)));
		}
		result.tiles.push_back(QRect(0, 0, tileWidth, height));
		result.height = height;
		return result;
	}
	const auto half = (width - skip) / 2;
	const auto secondLeft = half + skip;
	const auto secondWidth = width - secondLeft;
	if (count == 2) {
		// Two portraits side by side, a little taller than wide at most.
		auto height = int(std::lround(half * 1.25));
		if (maxHeight > 0) {
			height = std::min(height, maxHeight);
		}
		result.tiles.push_back(QRect(0, 0, half, height));
		result.tiles.push_back(QRect(secondLeft, 0, secondWidth, height));
		result.height = height;
		return result;
	}
	auto height = int(std::lround(width * 9. / 16.));
	if (maxHeight > 0) {
		height = std::min(height, maxHeight);
	}
	const auto rowHeight = (height - skip) / 2;
	const auto secondTop = rowHeight + skip;
	const auto secondHeight = height - secondTop;
	if (count == 3) {
		result.tiles.push_back(QRect(0, 0, half, height));
		result.tiles.push_back(QRect(secondLeft, 0, secondWidth, rowHeight));
		result.tiles.push_back(
			QRect(secondLeft, secondTop, secondWidth, secondHeight));
		result.height = height;
		return result;
	}
	result.tiles.push_back(QRect(0, 0, half, rowHeight));
	result.tiles.push_back(QRect(secondLeft, 0, secondWidth, rowHeight));
	result.tiles.push_back(QRect(0, secondTop, half, secondHeight));
	result.tiles.push_back(
		QRect(secondLeft, secondTop, secondWidth, secondHeight));
	result.height = height;
	result.more = count - kMaxGridTiles;
	return result;
}

QVector<int> PollPercents(const QVector<qint64> &votes) {
	auto result = QVector<int>(votes.size(), 0);
	auto total = qint64(0);
	for (const auto value : votes) {
		total += std::max(value, qint64(0));
	}
	if (total <= 0) {
		return result;
	}
	struct Rest {
		int index = 0;
		qint64 remainder = 0;
	};
	auto rests = std::vector<Rest>();
	auto sum = 0;
	for (auto i = 0; i != int(votes.size()); ++i) {
		const auto value = std::max(votes[i], qint64(0));
		result[i] = int(value * 100 / total);
		sum += result[i];
		rests.push_back({ i, (value * 100) % total });
	}
	std::stable_sort(rests.begin(), rests.end(), [](const Rest &a, const Rest &b) {
		return a.remainder > b.remainder;
	});
	for (auto i = 0; sum < 100 && i != int(rests.size()); ++i, ++sum) {
		++result[rests[i].index];
	}
	return result;
}

Clamp ClampLines(int totalLines, int maxLines, int minHidden) {
	if (maxLines <= 0 || totalLines < maxLines + std::max(minHidden, 1)) {
		return { std::max(totalLines, 0), false };
	}
	return { maxLines, true };
}

int TextLength(const QString &text) {
	auto result = 0;
	for (auto i = 0; i < text.size(); ++i) {
		if (text.at(i).isHighSurrogate()
			&& i + 1 < text.size()
			&& text.at(i + 1).isLowSurrogate()) {
			++i;
		}
		++result;
	}
	return result;
}

int ReadingMinutes(const QStringList &parts) {
	static const auto kSpaces = QRegularExpression(QStringLiteral("\\s+"));
	auto words = 0;
	for (const auto &part : parts) {
		words += int(part.split(kSpaces, Qt::SkipEmptyParts).size());
	}
	return std::max(1, int(std::lround(double(words) / kWordsPerMinute)));
}

ComposeProblem ValidateCompose(
		const ComposeDraft &draft,
		const ComposeLimits &limits) {
	const auto length = TextLength(draft.text.trimmed());
	const auto parts = CleanThreadParts(draft.parts);
	if (draft.hasPoll && draft.mediaCount > 0) {
		return ComposeProblem::PollWithMedia;
	}
	if (draft.mediaCount > limits.maxMedia) {
		return ComposeProblem::TooManyMedia;
	}
	if (length > limits.maxText) {
		return ComposeProblem::TooLong;
	}
	for (const auto &part : parts) {
		if (TextLength(part) > limits.maxText) {
			return ComposeProblem::PartTooLong;
		}
	}
	if (1 + int(parts.size()) > limits.maxThreadParts) {
		return ComposeProblem::TooManyParts;
	}
	if (draft.hasPoll) {
		auto filled = 0;
		for (const auto &option : draft.pollOptions) {
			if (!option.trimmed().isEmpty()) {
				++filled;
			}
		}
		if (filled < limits.pollMinOptions || filled > limits.pollMaxOptions) {
			return ComposeProblem::PollOptions;
		}
	}
	const auto hasContent = (length > 0)
		|| (draft.mediaCount > 0)
		|| (draft.kind == ComposeKind::Invite);
	return hasContent ? ComposeProblem::None : ComposeProblem::Empty;
}

QString ComposeType(const ComposeDraft &draft) {
	if (draft.kind == ComposeKind::Invite) {
		return QStringLiteral("conversation_invite");
	} else if (draft.kind == ComposeKind::Quote) {
		return QStringLiteral("quote");
	} else if (!CleanThreadParts(draft.parts).isEmpty()) {
		return QStringLiteral("thread");
	} else if (draft.hasPoll) {
		return QStringLiteral("poll");
	} else if (draft.mediaCount > 0) {
		return QStringLiteral("media");
	}
	return QStringLiteral("text");
}

QStringList CleanThreadParts(const QStringList &parts) {
	auto result = QStringList();
	for (const auto &part : parts) {
		const auto trimmed = part.trimmed();
		if (!trimmed.isEmpty()) {
			result.push_back(trimmed);
		}
	}
	return result;
}

int Remaining(const QString &text, int limit) {
	return limit - TextLength(text);
}

QStringList SplitIntoThread(const QString &text, int limit, int maxParts) {
	auto result = QStringList();
	auto rest = text.trimmed();
	if (limit <= 0 || maxParts <= 0) {
		if (!rest.isEmpty()) {
			result.push_back(rest);
		}
		return result;
	}
	while (!rest.isEmpty()) {
		if (int(result.size()) == maxParts - 1 || TextLength(rest) <= limit) {
			result.push_back(rest);
			break;
		}
		// [0, hard) is the longest prefix that fits.
		const auto hard = AdvanceCodePoints(rest, 0, limit);
		const auto half = hard / 2;
		auto cut = -1;
		// A paragraph break in the second half of the part.
		const auto paragraph = rest.lastIndexOf(QChar('\n'), hard - 1);
		if (paragraph >= half) {
			cut = int(paragraph);
		}
		if (cut < 0) {
			// The end of a sentence followed by a space.
			for (auto i = hard - 1; i >= half; --i) {
				if (IsSentenceEnd(rest.at(i))
					&& (i + 1 >= rest.size() || rest.at(i + 1).isSpace())) {
					cut = i + 1;
					break;
				}
			}
		}
		if (cut < 0) {
			for (auto i = hard; i > 0; --i) {
				if (i < rest.size() && rest.at(i).isSpace()) {
					cut = i;
					break;
				}
			}
		}
		if (cut <= 0) {
			cut = hard;
		}
		const auto part = rest.left(cut).trimmed();
		if (!part.isEmpty()) {
			result.push_back(part);
		}
		rest = rest.mid(cut).trimmed();
	}
	return result;
}

QVector<ReplyRow> ArrangeReplies(
		const QVector<ReplyNode> &all,
		const QString &rootId,
		bool newestFirst,
		const QStringList &first) {
	auto byParent = QHash<QString, QVector<int>>();
	auto known = QSet<QString>();
	for (const auto &node : all) {
		known.insert(node.id);
	}
	for (auto i = 0; i != int(all.size()); ++i) {
		const auto &node = all[i];
		// A reply whose parent did not come with the page hangs under the root.
		const auto key = (node.replyTo.isEmpty() || !known.contains(node.replyTo))
			? rootId
			: node.replyTo;
		byParent[key].push_back(i);
	}
	{
		auto &top = byParent[rootId];
		if (newestFirst) {
			std::stable_sort(top.begin(), top.end(), [&](int a, int b) {
				return all[a].createdAt > all[b].createdAt;
			});
		}
		if (!first.isEmpty()) {
			// (stable: the others keep the order of the sort)
			const auto rank = [&](int index) {
				const auto at = first.indexOf(all[index].id);
				return (at < 0) ? int(first.size()) : int(at);
			};
			std::stable_sort(top.begin(), top.end(), [&](int a, int b) {
				return rank(a) < rank(b);
			});
		}
	}
	auto hasLiving = std::function<bool(const QString &, int)>();
	hasLiving = [&](const QString &id, int guard) {
		if (guard > 64) {
			return false;
		}
		for (const auto index : byParent.value(id)) {
			if (!all[index].deleted || hasLiving(all[index].id, guard + 1)) {
				return true;
			}
		}
		return false;
	};
	auto rows = QVector<ReplyRow>();
	auto walk = std::function<void(const QString &, int, int)>();
	walk = [&](const QString &parent, int depth, int guard) {
		if (guard > 64) {
			return;
		}
		for (const auto index : byParent.value(parent)) {
			const auto &node = all[index];
			if (node.deleted && !hasLiving(node.id, 0)) {
				continue;
			}
			auto row = ReplyRow();
			row.id = node.id;
			row.depth = depth;
			row.parent = (depth == 1) ? parent : QString();
			rows.push_back(row);
			walk(node.id, 1, guard + 1);
		}
	};
	walk(rootId, 0, 0);
	for (auto i = 0; i + 1 < int(rows.size()); ++i) {
		rows[i].line = (rows[i + 1].depth == 1);
	}
	return rows;
}

QStringList FocusPath(const QVector<ClimbStep> &climb) {
	if (climb.isEmpty() || !climb.front().comment || climb.front().deleted) {
		return {};
	}
	auto result = QStringList();
	for (const auto &step : climb) {
		if (!step.comment) {
			break;
		} else if (!result.contains(step.id)) {
			result.push_front(step.id);
		}
	}
	return result;
}

QStringList PinThread(const QStringList &page, const QStringList &path) {
	const auto onPage = std::all_of(path.cbegin(), path.cend(), [&](
			const QString &id) {
		return page.contains(id);
	});
	if (path.isEmpty() || onPage) {
		return page;
	}
	auto result = path;
	for (const auto &id : page) {
		if (!result.contains(id)) {
			result.push_back(id);
		}
	}
	return result;
}

CommentTarget CommentTargetOf(
		const QString &kind,
		const QMap<QString, QString> &refs) {
	const auto ref = [&](const char *name) {
		return refs.value(QString::fromLatin1(name)).trimmed();
	};
	auto result = CommentTarget();
	if (kind == u"reply") {
		result.post = ref("reply");
		if (result.post.isEmpty()) {
			// An older item: what was answered.
			result.post = ref("post");
		}
	} else if (kind == u"mention") {
		result.post = ref("post");
	} else {
		return {};
	}
	if (result.post.isEmpty()) {
		result.post = ref("root");
	}
	const auto root = ref("root");
	if (!result.post.isEmpty() && root != result.post) {
		result.rootHint = root;
	}
	return result;
}

bool AnswersComment(const QMap<QString, QString> &refs) {
	const auto root = refs.value(QStringLiteral("root")).trimmed();
	if (root.isEmpty()) {
		return false;
	}
	auto parent = refs.value(QStringLiteral("parent")).trimmed();
	if (parent.isEmpty()) {
		parent = refs.value(QStringLiteral("post")).trimmed();
	}
	return !parent.isEmpty() && (parent != root);
}

void ViewTracker::setVisible(const QStringList &ids, qint64 now) {
	auto next = QHash<QString, qint64>();
	for (const auto &id : ids) {
		if (_reported.contains(id)) {
			continue;
		}
		const auto i = _since.constFind(id);
		next.insert(id, (i != _since.cend()) ? *i : now);
	}
	_since = std::move(next);
}

QStringList ViewTracker::takeSeen(qint64 now) {
	auto result = QStringList();
	for (auto i = _since.begin(); i != _since.end();) {
		if (now - i.value() >= kDwellMs) {
			result.push_back(i.key());
			_reported.insert(i.key());
			i = _since.erase(i);
		} else {
			++i;
		}
	}
	result.sort();
	return result;
}

int ViewTracker::nextCheckIn(qint64 now) const {
	auto result = -1;
	for (const auto since : _since) {
		const auto left = int(std::max(since + kDwellMs - now, qint64(0)));
		if (result < 0 || left < result) {
			result = left;
		}
	}
	return result;
}

void ViewTracker::reset() {
	_since.clear();
	_reported.clear();
}

BeaconQueue::BeaconQueue(int intervalMs, int batchSize)
: _intervalMs(intervalMs)
, _batchSize(std::max(batchSize, 1)) {
}

bool BeaconQueue::push(const QStringList &ids) {
	const auto wasEmpty = _queue.isEmpty();
	for (const auto &id : ids) {
		if (!_known.contains(id)) {
			_known.insert(id);
			_queue.push_back(id);
		}
	}
	return wasEmpty && !_queue.isEmpty();
}

QStringList BeaconQueue::takeBatch() {
	const auto batch = _queue.mid(0, _batchSize);
	_queue = _queue.mid(batch.size());
	return batch;
}

void CountsFreshness::noteFresh(const QStringList &ids, qint64 now) {
	for (const auto &id : ids) {
		_at.insert(id, now);
	}
}

QStringList CountsFreshness::takeStale(const QStringList &appeared, qint64 now) {
	auto result = QStringList();
	for (const auto &id : appeared) {
		const auto i = _at.find(id);
		if (i == _at.end()) {
			// Never seen a payload of it: nothing to compare with, it is
			// fresh as of now.
			_at.insert(id, now);
		} else if (now - i.value() > kMaxAgeMs) {
			i.value() = now;
			result.push_back(id);
		}
	}
	return result;
}

void CountsFreshness::forget(const QString &id) {
	_at.remove(id);
}

} // namespace Eli::Logic
