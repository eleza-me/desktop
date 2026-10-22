#include "history/history_compose_logic.h"

#include "ui/text/text_entity.h"

#include <algorithm>

namespace History {
namespace {

[[nodiscard]] bool IsSpoiler(const Data::Entity &entity) {
	return entity.type == u"spoiler";
}

[[nodiscard]] bool InsidePair(const QString &text, int position) {
	return position > 0
		&& position < text.size()
		&& text[position].isLowSurrogate()
		&& text[position - 1].isHighSurrogate();
}

[[nodiscard]] bool EntitiesEqual(
		const QVector<Data::Entity> &a,
		const QVector<Data::Entity> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != a.size(); ++i) {
		if (a[i].type != b[i].type
			|| a[i].offset != b[i].offset
			|| a[i].length != b[i].length
			|| a[i].url != b[i].url) {
			return false;
		}
	}
	return true;
}

} // namespace

TextEdit DiffTexts(const QString &was, const QString &now, int hint) {
	const auto wasSize = int(was.size());
	const auto nowSize = int(now.size());
	const auto limit = std::min(wasSize, nowSize);
	auto prefix = 0;
	while (prefix < limit && was[prefix] == now[prefix]) {
		++prefix;
	}
	auto suffix = 0;
	while (suffix < limit - prefix
		&& was[wasSize - 1 - suffix] == now[nowSize - 1 - suffix]) {
		++suffix;
	}
	// Whole characters only.
	if (InsidePair(was, prefix) || InsidePair(now, prefix)) {
		--prefix;
	}
	while (suffix > 0
		&& (InsidePair(was, wasSize - suffix)
			|| InsidePair(now, nowSize - suffix))) {
		--suffix;
	}
	auto result = TextEdit();
	result.position = prefix;
	result.removed = wasSize - prefix - suffix;
	result.added = nowSize - prefix - suffix;
	// Through repeated characters the same edit fits at several places
	// ("a" typed inside "aaa"): the editor knows which one it was.
	if (hint >= 0
		&& hint < prefix
		&& !InsidePair(was, hint)
		&& !InsidePair(now, hint)
		&& QStringView(was).mid(hint + result.removed)
			== QStringView(now).mid(hint + result.added)) {
		result.position = hint;
	}
	return result;
}

QVector<Data::Entity> AdjustEntities(
		QVector<Data::Entity> entities,
		const TextEdit &edit) {
	if (edit.empty()) {
		return entities;
	}
	const auto from = edit.position;
	const auto till = edit.position + edit.removed;
	const auto delta = edit.added - edit.removed;
	auto result = QVector<Data::Entity>();
	result.reserve(entities.size());
	for (auto &entity : entities) {
		const auto start = entity.offset;
		const auto end = entity.offset + entity.length;
		if (end <= from) {
			// Before the edit (typing right after a range does not extend it).
		} else if (start >= till) {
			// After the edit (typing right before a range does not extend it).
			entity.offset += delta;
		} else if (start < from && end > till) {
			// The edit is inside: what was typed belongs to the range.
			entity.length += delta;
		} else if (start < from) {
			// The tail was removed.
			entity.length = from - start;
		} else if (end > till) {
			// The head was removed; the replacement is not part of the range.
			entity.offset = from + edit.added;
			entity.length = end - till;
		} else {
			// Removed completely.
			continue;
		}
		if (entity.length > 0) {
			result.push_back(std::move(entity));
		}
	}
	return result;
}

QVector<Data::Entity> AdjustEntities(
		QVector<Data::Entity> entities,
		const QString &was,
		const QString &now,
		int hint) {
	if (entities.isEmpty() || was == now) {
		return entities;
	}
	return NormalizeEntities(
		now,
		AdjustEntities(std::move(entities), DiffTexts(was, now, hint)));
}

QVector<Data::Entity> NormalizeEntities(
		const QString &text,
		QVector<Data::Entity> entities) {
	const auto size = int(text.size());
	auto result = QVector<Data::Entity>();
	result.reserve(entities.size());
	for (auto &entity : entities) {
		auto from = std::clamp(entity.offset, 0, size);
		auto till = std::clamp(
			int(std::min(qint64(entity.offset) + entity.length, qint64(size))),
			from,
			size);
		if (InsidePair(text, from)) {
			--from;
		}
		if (InsidePair(text, till)) {
			++till;
		}
		if (till <= from) {
			continue;
		}
		entity.offset = from;
		entity.length = till - from;
		result.push_back(std::move(entity));
	}
	std::stable_sort(
		result.begin(),
		result.end(),
		[](const Data::Entity &a, const Data::Entity &b) {
			return (a.offset < b.offset)
				|| (a.offset == b.offset && a.length > b.length);
		});
	// Spoilers that overlap or touch become one.
	auto merged = QVector<Data::Entity>();
	merged.reserve(result.size());
	auto lastSpoiler = -1;
	for (auto &entity : result) {
		if (IsSpoiler(entity) && lastSpoiler >= 0) {
			auto &previous = merged[lastSpoiler];
			if (entity.offset <= previous.offset + previous.length) {
				const auto end = std::max(
					previous.offset + previous.length,
					entity.offset + entity.length);
				previous.length = end - previous.offset;
				continue;
			}
		}
		if (IsSpoiler(entity)) {
			lastSpoiler = int(merged.size());
		}
		merged.push_back(std::move(entity));
	}
	return merged;
}

bool RangeIsSpoiler(
		const QVector<Data::Entity> &entities,
		int from,
		int till) {
	if (till <= from) {
		return false;
	}
	// The spoilers are few: walk the range through them.
	auto covered = from;
	auto progress = true;
	while (covered < till && progress) {
		progress = false;
		for (const auto &entity : entities) {
			if (IsSpoiler(entity)
				&& entity.offset <= covered
				&& entity.offset + entity.length > covered) {
				covered = entity.offset + entity.length;
				progress = true;
			}
		}
	}
	return covered >= till;
}

QVector<Data::Entity> ToggleSpoiler(
		const QString &text,
		QVector<Data::Entity> entities,
		int from,
		int till) {
	const auto size = int(text.size());
	from = std::clamp(from, 0, size);
	till = std::clamp(till, from, size);
	if (InsidePair(text, from)) {
		--from;
	}
	if (InsidePair(text, till)) {
		++till;
	}
	if (till <= from) {
		return entities;
	}
	if (!RangeIsSpoiler(entities, from, till)) {
		auto added = Data::Entity();
		added.type = u"spoiler"_s;
		added.offset = from;
		added.length = till - from;
		entities.push_back(std::move(added));
		return NormalizeEntities(text, std::move(entities));
	}
	auto result = QVector<Data::Entity>();
	result.reserve(entities.size() + 1);
	for (auto &entity : entities) {
		const auto start = entity.offset;
		const auto end = entity.offset + entity.length;
		if (!IsSpoiler(entity) || end <= from || start >= till) {
			result.push_back(std::move(entity));
			continue;
		}
		if (start < from) {
			auto left = entity;
			left.length = from - start;
			result.push_back(std::move(left));
		}
		if (end > till) {
			auto right = entity;
			right.offset = till;
			right.length = end - till;
			result.push_back(std::move(right));
		}
	}
	return NormalizeEntities(text, std::move(result));
}

QVector<Data::Entity> EntitiesForSend(
		const QVector<Data::Entity> &spoilers,
		const QVector<Data::Entity> &others) {
	auto result = spoilers;
	result.reserve(spoilers.size() + others.size());
	for (const auto &entity : others) {
		const auto from = entity.offset;
		const auto till = entity.offset + entity.length;
		const auto overlaps = std::any_of(
			result.begin(),
			result.end(),
			[&](const Data::Entity &taken) {
				return (taken.offset < till)
					&& (from < taken.offset + taken.length);
			});
		if (!overlaps && till > from) {
			result.push_back(entity);
		}
	}
	return result;
}

PreparedText PrepareForSend(
		const QString &text,
		const QVector<Data::Entity> &entities,
		int maxEntities) {
	auto result = PreparedText();
	const auto size = int(text.size());
	auto from = 0;
	auto till = size;
	while (from < till && text[from].isSpace()) {
		++from;
	}
	while (till > from && text[till - 1].isSpace()) {
		--till;
	}
	if (till <= from) {
		return result;
	}
	result.text = text.mid(from, till - from);
	for (auto entity : entities) {
		const auto start = std::max(entity.offset, from);
		const auto end = std::min(entity.offset + entity.length, till);
		if (end <= start) {
			continue;
		}
		entity.offset = start - from;
		entity.length = end - start;
		result.entities.push_back(std::move(entity));
	}
	result.entities = NormalizeEntities(result.text, std::move(result.entities));
	if (maxEntities > 0 && result.entities.size() > maxEntities) {
		result.entities.resize(maxEntities);
	}
	return result;
}

std::vector<PreparedText> SplitForSend(
		const PreparedText &prepared,
		int maxLength) {
	auto result = std::vector<PreparedText>();
	if (prepared.empty()) {
		return result;
	} else if (maxLength <= 0 || prepared.text.size() <= maxLength) {
		result.push_back(prepared);
		return result;
	}
	const auto &text = prepared.text;
	const auto size = int(text.size());
	auto from = 0;
	while (from < size) {
		auto till = std::min(from + maxLength, size);
		if (till < size) {
			// Prefer a line break, then a space, in the second half of the part.
			const auto floor = from + maxLength / 2;
			auto cut = -1;
			for (auto i = till; i > floor; --i) {
				if (text[i - 1] == QLatin1Char('\n')) {
					cut = i;
					break;
				}
			}
			if (cut < 0) {
				for (auto i = till; i > floor; --i) {
					if (text[i - 1].isSpace()) {
						cut = i;
						break;
					}
				}
			}
			if (cut > 0) {
				till = cut;
			} else if (InsidePair(text, till)) {
				--till;
			}
		}
		auto shifted = QVector<Data::Entity>();
		for (auto entity : prepared.entities) {
			const auto start = std::max(entity.offset, from);
			const auto end = std::min(entity.offset + entity.length, till);
			if (end > start) {
				entity.offset = start - from;
				entity.length = end - start;
				shifted.push_back(std::move(entity));
			}
		}
		auto part = PrepareForSend(text.mid(from, till - from), shifted);
		if (!part.empty()) {
			result.push_back(std::move(part));
		}
		from = till;
	}
	return result;
}

QString FirstLinkForPreview(
		const QString &text,
		const QVector<Data::Entity> &entities) {
	const auto parsed = Ui::Text::ParseEntities(text, Ui::Text::ParseLinks);
	for (const auto &entity : parsed.entities) {
		if (entity.type != Ui::Text::EntityType::Url) {
			continue;
		} else if (RangeIsSpoiler(
				entities,
				entity.offset,
				entity.offset + entity.length)) {
			continue;
		}
		const auto link = text.mid(entity.offset, entity.length);
		return link.contains(u"://"_s) ? link : (u"https://"_s + link);
	}
	return QString();
}

bool SameDraft(const Data::Draft &a, const Data::Draft &b) {
	return (a.text == b.text)
		&& (a.replyToSeq == b.replyToSeq)
		&& EntitiesEqual(a.entities, b.entities);
}

DraftSaver::DraftSaver(Delegate delegate)
: _delegate(std::move(delegate)) {
}

void DraftSaver::reset(const Data::Draft &loaded) {
	_stored = loaded;
	_current = loaded;
	_dirty = false;
	_changedAt = 0;
	if (_delegate.disarm) {
		_delegate.disarm();
	}
}

void DraftSaver::changed(const Data::Draft &current, qint64 nowMs) {
	if (_suspended) {
		return;
	}
	_current = current;
	_changedAt = nowMs;
	_dirty = !SameDraft(_current, _stored);
	if (_dirty) {
		if (_delegate.arm) {
			_delegate.arm(kDebounceMs);
		}
	} else if (_delegate.disarm) {
		_delegate.disarm();
	}
}

void DraftSaver::timeout(qint64 nowMs) {
	if (!_dirty || _suspended) {
		return;
	}
	const auto passed = nowMs - _changedAt;
	if (passed < kDebounceMs) {
		// Fired early (a keystroke re-armed it meanwhile): wait for the rest.
		if (_delegate.arm) {
			_delegate.arm(kDebounceMs - passed);
		}
		return;
	}
	store();
}

void DraftSaver::flush() {
	if (_delegate.disarm) {
		_delegate.disarm();
	}
	if (_dirty && !_suspended) {
		store();
	}
}

void DraftSaver::sent() {
	if (_delegate.disarm) {
		_delegate.disarm();
	}
	const auto hadStored = !_stored.empty();
	_stored = Data::Draft();
	_current = Data::Draft();
	_dirty = false;
	if (hadStored && _delegate.clear) {
		_delegate.clear();
	}
}

void DraftSaver::setSuspended(bool suspended) {
	if (_suspended == suspended) {
		return;
	}
	_suspended = suspended;
	if (_suspended && _delegate.disarm) {
		_delegate.disarm();
	}
}

bool DraftSaver::acceptsRemote(
		const std::optional<Data::Draft> &remote,
		bool focused) const {
	if (focused || _dirty || _suspended) {
		return false;
	}
	return !SameDraft(remote.value_or(Data::Draft()), _current);
}

void DraftSaver::store() {
	_dirty = false;
	auto draft = _current;
	if (draft.text.trimmed().isEmpty()) {
		draft = Data::Draft();
	}
	const auto same = SameDraft(draft, _stored);
	_stored = draft;
	if (same) {
		return;
	} else if (draft.empty()) {
		if (_delegate.clear) {
			_delegate.clear();
		}
	} else if (_delegate.save) {
		_delegate.save(draft);
	}
}

} // namespace History
