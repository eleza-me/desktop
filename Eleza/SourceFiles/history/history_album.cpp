#include "history/history_album.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace History {
namespace {

// Half up, as the other apps round.
[[nodiscard]] double Rnd(double x) {
	return std::floor(x + 0.5);
}

[[nodiscard]] char KindOf(double ratio) {
	return (ratio > 1.2) ? 'w' : (ratio < 0.8) ? 'n' : 'q';
}

[[nodiscard]] AlbumFrame Frame(double x, double y, double w, double h, int sides) {
	return AlbumFrame{ x, y, w, h, sides };
}

[[nodiscard]] AlbumFrame ScaledX(AlbumFrame frame, double k) {
	frame.x *= k;
	frame.w *= k;
	return frame;
}

constexpr auto L = int(kAlbumSideLeft);
constexpr auto T = int(kAlbumSideTop);
constexpr auto R = int(kAlbumSideRight);
constexpr auto B = int(kAlbumSideBottom);

// Five and more items: 2 to 4 rows of at most 3 items each (the middle row
// of three may hold 4 narrow ones); the split whose total height is closest
// to the box's wins, rows not getting fewer items going down and no row too
// low preferred.
[[nodiscard]] std::vector<AlbumFrame> Generic(const std::vector<double> &rs, double avg) {
	const auto n = int(rs.size());
	auto cropped = std::vector<double>(n);
	for (auto i = 0; i != n; ++i) {
		cropped[i] = (avg > 1.1) ? std::max(1., rs[i]) : std::min(1., rs[i]);
	}
	auto tries = std::vector<std::vector<int>>();
	for (auto a = 1; a < n; ++a) {
		if (a <= 3 && n - a <= 3) {
			tries.push_back({ a, n - a });
		}
	}
	for (auto a = 1; a < n - 1; ++a) {
		for (auto b = 1; b < n - a; ++b) {
			const auto c = n - a - b;
			if (a <= 3 && b <= ((avg < 0.85) ? 4 : 3) && c >= 1 && c <= 3) {
				tries.push_back({ a, b, c });
			}
		}
	}
	for (auto a = 1; a < n - 2; ++a) {
		for (auto b = 1; b < n - a - 1; ++b) {
			for (auto c = 1; c < n - a - b; ++c) {
				const auto d = n - a - b - c;
				if (a <= 3 && b <= 3 && c <= 3 && d >= 1 && d <= 3) {
					tries.push_back({ a, b, c, d });
				}
			}
		}
	}
	auto bestDiff = 0.;
	auto best = -1;
	auto bestHeights = std::vector<double>();
	for (auto t = 0; t != int(tries.size()); ++t) {
		const auto &counts = tries[t];
		auto heights = std::vector<double>(counts.size());
		auto i = 0;
		auto total = 0.;
		auto low = std::numeric_limits<double>::max();
		for (auto r = 0; r != int(counts.size()); ++r) {
			auto s = 0.;
			for (auto k = 0; k != counts[r]; ++k) {
				s += cropped[i + k];
			}
			heights[r] = kAlbumMaxWidth / s;
			total += heights[r];
			low = std::min(low, heights[r]);
			i += counts[r];
		}
		auto diff = std::abs(total - kAlbumMaxHeight);
		for (auto k = 0; k + 1 < int(counts.size()); ++k) {
			if (counts[k] > counts[k + 1]) {
				diff *= 1.5;
				break;
			}
		}
		if (low < kAlbumMinHeight) {
			diff *= 1.5;
		}
		if (best < 0 || diff < bestDiff - 1e-9) {
			bestDiff = diff;
			best = t;
			bestHeights = heights;
		}
	}
	auto out = std::vector<AlbumFrame>();
	if (best < 0) {
		return out;
	}
	const auto &counts = tries[best];
	auto total = 0.;
	for (const auto h : bestHeights) {
		total += h;
	}
	const auto scale = std::min(1., kAlbumMaxHeight / total); // never taller than the box
	auto i = 0;
	auto y = 0.;
	for (auto row = 0; row != int(counts.size()); ++row) {
		const auto c = counts[row];
		const auto h = Rnd(bestHeights[row] * scale);
		auto x = 0.;
		auto widths = std::vector<double>(c);
		auto sum = 0.;
		for (auto k = 0; k != c; ++k) {
			widths[k] = cropped[i + k] * h;
			sum += widths[k];
		}
		const auto kScale = kAlbumMaxWidth / sum; // the row fills the width exactly
		for (auto k = 0; k != c; ++k) {
			const auto w = (k == c - 1) ? (kAlbumMaxWidth - x) : Rnd(widths[k] * kScale);
			auto f = 0;
			if (k == 0) {
				f |= L;
			}
			if (k == c - 1) {
				f |= R;
			}
			if (row == 0) {
				f |= T;
			}
			if (row == int(counts.size()) - 1) {
				f |= B;
			}
			out.push_back(Frame(x, y, w, h, f));
			x += w;
		}
		y += h;
		i += c;
	}
	return out;
}

[[nodiscard]] bool Albumable(const Data::Message &message) {
	return (message.seq > 0 || message.mine)
		&& !message.underReview
		&& !message.deleted
		&& (AlbumFamilyOf(message.kind) == AlbumFamily::Visual)
		&& !AlbumKey(message).isEmpty();
}

} // namespace

std::vector<AlbumFrame> AlbumLayout(const std::vector<double> &ratios) {
	const auto n = int(ratios.size());
	auto out = std::vector<AlbumFrame>();
	if (!n) {
		return out;
	}
	auto rs = std::vector<double>(n);
	for (auto i = 0; i != n; ++i) {
		const auto r = ratios[i];
		rs[i] = (r > 0. && !std::isnan(r) && !std::isinf(r)) ? r : 1.;
	}
	if (n == 1) {
		const auto h = std::min(
			kAlbumMaxHeight,
			std::max(kAlbumMinHeight, kAlbumMaxWidth / rs[0]));
		out.push_back(Frame(0, 0, kAlbumMaxWidth, h, L | T | R | B));
		return out;
	}
	auto props = std::string();
	auto sum = 0.;
	for (const auto r : rs) {
		props.push_back(KindOf(r));
		sum += r;
	}
	const auto avg = sum / n;
	const auto maxAr = kAlbumMaxWidth / kAlbumMaxHeight;
	const auto W = kAlbumMaxWidth;
	const auto H = kAlbumMaxHeight;
	if (n == 2) {
		const auto r0 = rs[0];
		const auto r1 = rs[1];
		if (props == "ww" && avg > 1.4 * maxAr && std::abs(r0 - r1) < 0.2) {
			// Two wide ones: one above the other.
			const auto h = Rnd(std::min(std::min(W / r0, W / r1), H / 2));
			out.push_back(Frame(0, 0, W, h, L | T | R));
			out.push_back(Frame(0, h, W, h, L | R | B));
		} else if (props == "ww" || props == "qq") {
			const auto w = W / 2;
			const auto h = Rnd(std::min(std::min(w / r0, w / r1), H));
			out.push_back(Frame(0, 0, w, h, L | T | B));
			out.push_back(Frame(w, 0, w, h, T | R | B));
		} else {
			const auto w1 = std::max(0.4 * W, Rnd(W / r0 / (1 / r0 + 1 / r1)));
			const auto w0 = W - w1;
			const auto h = std::min(H, Rnd(std::min(w0 / r0, w1 / r1)));
			out.push_back(Frame(0, 0, w0, h, L | T | B));
			out.push_back(Frame(w0, 0, w1, h, T | R | B));
		}
		return out;
	}
	if (n == 3) {
		const auto r0 = rs[0];
		const auto r1 = rs[1];
		const auto r2 = rs[2];
		if (props[0] == 'n') {
			// A tall one on the side, two stacked next to it.
			const auto h2 = std::min(H * 0.5, Rnd(r1 * W / (r2 + r1)));
			const auto h1 = H - h2;
			const auto wr = std::max(
				kAlbumMinWidth,
				std::min(W * 0.5, Rnd(std::min(h2 * r2, h1 * r1))));
			const auto wl = Rnd(std::min(H * r0, W - wr));
			const auto scale = W / (wl + wr); // fill the width
			out.push_back(ScaledX(Frame(0, 0, wl, H, L | T | B), scale));
			out.push_back(ScaledX(Frame(wl, 0, wr, h1, T | R), scale));
			out.push_back(ScaledX(Frame(wl, h1, wr, h2, R | B), scale));
		} else {
			// A wide one on top, two under it.
			const auto h0 = Rnd(std::min(W / r0, H * 0.66));
			const auto w = W / 2;
			const auto h1 = std::min(H - h0, Rnd(std::min(w / r1, w / r2)));
			out.push_back(Frame(0, 0, W, h0, L | T | R));
			out.push_back(Frame(0, h0, w, h1, L | B));
			out.push_back(Frame(w, h0, w, h1, R | B));
		}
		return out;
	}
	if (n == 4) {
		const auto r0 = rs[0];
		const auto r1 = rs[1];
		const auto r2 = rs[2];
		const auto r3 = rs[3];
		if (props[0] == 'w') {
			// A wide one on top, three under it.
			const auto h0 = Rnd(std::min(W / r0, H * 0.66));
			auto h = Rnd(W / (1 / r1 + 1 / r2 + 1 / r3));
			auto w0 = Rnd(std::max(kAlbumMinWidth, std::min(W * 0.4, h * r1)));
			auto w2 = Rnd(std::max(std::max(kAlbumMinWidth, W * 0.33), h * r3));
			auto w1 = W - w0 - w2;
			if (w1 < kAlbumMinWidth) {
				// The middle one gets its minimum back from the widest.
				const auto d = kAlbumMinWidth - w1;
				if (w0 >= w2) {
					w0 -= d;
				} else {
					w2 -= d;
				}
				w1 = kAlbumMinWidth;
			}
			h = std::min(H - h0, h);
			out.push_back(Frame(0, 0, W, h0, L | T | R));
			out.push_back(Frame(0, h0, w0, h, L | B));
			out.push_back(Frame(w0, h0, w1, h, B));
			out.push_back(Frame(w0 + w1, h0, w2, h, R | B));
		} else {
			// A tall one on the side, three stacked next to it.
			auto w = Rnd(std::max(kAlbumMinWidth, H / (1 / r1 + 1 / r2 + 1 / r3)));
			w = std::min(w, W - kAlbumMinWidth);
			const auto h0 = std::min(0.33 * H, Rnd(w / r1));
			const auto h1 = std::min(0.33 * H, Rnd(w / r2));
			const auto h2 = H - h0 - h1;
			const auto wl = Rnd(std::min(H * r0, W - w));
			const auto scale = W / (wl + w);
			out.push_back(ScaledX(Frame(0, 0, wl, H, L | T | B), scale));
			out.push_back(ScaledX(Frame(wl, 0, w, h0, T | R), scale));
			out.push_back(ScaledX(Frame(wl, h0, w, h1, R), scale));
			out.push_back(ScaledX(Frame(wl, h0 + h1, w, h2, R | B), scale));
		}
		return out;
	}
	return Generic(rs, avg);
}

double AlbumHeight(const std::vector<AlbumFrame> &frames) {
	auto result = 0.;
	for (const auto &frame : frames) {
		result = std::max(result, frame.y + frame.h);
	}
	return result;
}

AlbumFamily AlbumFamilyOf(Data::MessageKind kind) {
	switch (kind) {
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
		return AlbumFamily::Visual;
	case Data::MessageKind::File:
		return AlbumFamily::File;
	case Data::MessageKind::Audio:
		return AlbumFamily::Audio;
	default:
		return AlbumFamily::None;
	}
}

QString AlbumKey(const Data::Message &message) {
	if (!message.album.isEmpty()) {
		return u"u:"_s + message.album.toLower();
	}
	const auto &id = message.clientMessageId;
	if (id.size() != 36 || id.mid(19, 3).compare(u"a1b"_s, Qt::CaseInsensitive) != 0) {
		return QString();
	}
	return id.left(23).toLower();
}

std::vector<AlbumRun> GroupAlbums(
		const QVector<Data::Message> &items,
		bool group,
		int maxItems) {
	auto result = std::vector<AlbumRun>();
	const auto max = std::max(maxItems, 2);
	const auto count = int(items.size());
	for (auto i = 0; i < count;) {
		const auto &head = items[i];
		if (!Albumable(head)) {
			++i;
			continue;
		}
		const auto key = AlbumKey(head);
		auto j = i + 1;
		while (j < count && j - i < max) {
			const auto &item = items[j];
			if (!Albumable(item)
				|| AlbumKey(item) != key
				|| item.mine != head.mine
				|| (group && !head.mine && item.sender != head.sender)) {
				break;
			}
			++j;
		}
		if (j - i >= 2) {
			result.push_back({ i, j - i });
		}
		i = j;
	}
	return result;
}

int AlbumCaptionIndex(const QVector<Data::Message> &items) {
	for (auto i = 0; i != int(items.size()); ++i) {
		if (!items[i].text.isEmpty()) {
			return i;
		}
	}
	return -1;
}

Data::Message AlbumHeadMessage(const QVector<Data::Message> &items) {
	if (items.isEmpty()) {
		return Data::Message();
	}
	auto result = items.front();
	const auto caption = AlbumCaptionIndex(items);
	if (caption >= 0) {
		const auto &from = items[caption];
		result.text = from.text;
		result.entities = from.entities;
		result.captionAbove = from.captionAbove;
		result.edited = from.edited;
	} else {
		result.text = QString();
		result.entities = {};
		result.captionAbove = false;
	}
	const auto &last = items.back();
	result.createdAt = last.createdAt;
	result.uploadProgress = -1.;
	if (result.mine) {
		auto status = last.status;
		for (const auto &item : items) {
			if (item.status == Data::MessageStatus::Failed) {
				status = Data::MessageStatus::Failed;
			} else if ((item.status == Data::MessageStatus::Sending
				|| item.status == Data::MessageStatus::Queued)
				&& status != Data::MessageStatus::Failed) {
				status = Data::MessageStatus::Sending;
			} else if (item.status == Data::MessageStatus::Sent
				&& status == Data::MessageStatus::Read) {
				// Read only when every item is.
				status = Data::MessageStatus::Sent;
			}
		}
		result.status = status;
		if (status != Data::MessageStatus::Failed) {
			result.errorCode = QString();
			result.errorText = QString();
		}
	}
	return result;
}

AlbumSendPlan PlanAlbumSend(
		const std::vector<AlbumFamily> &families,
		bool grouped,
		int maxItems) {
	auto result = AlbumSendPlan();
	const auto n = int(families.size());
	result.albums.assign(n, -1);
	result.captionOnFirst = (n > 0) && std::all_of(
		families.begin(),
		families.end(),
		[](AlbumFamily family) { return family == AlbumFamily::Visual; });
	if (!grouped || n < 2) {
		return result;
	}
	const auto max = std::max(maxItems, 2);
	for (auto i = 0; i < n;) {
		if (families[i] == AlbumFamily::None) {
			++i;
			continue;
		}
		auto j = i + 1;
		while (j < n && families[j] == families[i] && j - i < max) {
			++j;
		}
		if (j - i >= 2) {
			for (auto k = i; k != j; ++k) {
				result.albums[k] = result.albumCount;
			}
			++result.albumCount;
		}
		i = j;
	}
	return result;
}

std::vector<int> ForwardAlbumGroups(
		const QVector<Data::Message> &messages,
		int maxItems) {
	const auto n = int(messages.size());
	auto result = std::vector<int>(n, -1);
	const auto max = std::max(maxItems, 2);
	auto count = 0;
	for (auto i = 0; i < n;) {
		const auto key = AlbumKey(messages[i]);
		const auto family = AlbumFamilyOf(messages[i].kind);
		if (key.isEmpty() || family == AlbumFamily::None) {
			++i;
			continue;
		}
		auto j = i + 1;
		while (j < n
			&& j - i < max
			&& AlbumKey(messages[j]) == key
			&& AlbumFamilyOf(messages[j].kind) == family) {
			++j;
		}
		if (j - i >= 2) {
			for (auto k = i; k != j; ++k) {
				result[k] = count;
			}
			++count;
		}
		i = j;
	}
	return result;
}

} // namespace History
