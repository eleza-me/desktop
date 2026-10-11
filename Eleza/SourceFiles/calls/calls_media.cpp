#include "calls/calls_media.h"

#include "calls/calls_ffi.h"

#include <QtCore/QJsonArray>

#include <algorithm>
#include <cmath>

namespace Calls {

MediaJoin MediaJoin::FromJson(const QJsonObject &join) {
	auto result = MediaJoin();
	result.url = join.value(QLatin1StringView("sfu_url")).toString();
	result.token = join.value(QLatin1StringView("token")).toString();
	// Absent in the answer of an older server: every call is relayed.
	result.relayOnly = join.value(QLatin1StringView("relay_only")).toBool(true);
	const auto turn = join.value(QLatin1StringView("turn")).toObject();
	for (const auto &url : turn.value(QLatin1StringView("urls")).toArray()) {
		const auto text = url.toString();
		// Only what an ICE server can be: nothing else reaches the library.
		if (text.startsWith(QLatin1StringView("turn:"))
			|| text.startsWith(QLatin1StringView("turns:"))
			|| text.startsWith(QLatin1StringView("stun:"))) {
			result.turnUrls.push_back(text);
		}
	}
	if (!result.turnUrls.isEmpty()) {
		result.turnUsername = turn.value(QLatin1StringView("username")).toString();
		result.turnCredential = turn.value(QLatin1StringView("credential")).toString();
	}
	return result;
}

double EstimateMos(double rttMs, double jitterMs, double lossPercent) {
	const auto latency = std::max(rttMs, 0.) / 2.
		+ std::max(jitterMs, 0.) * 2.
		+ 10.;
	auto r = 93.2 - ((latency < 160.)
		? (latency / 40.)
		: ((latency - 120.) / 10.));
	r -= std::clamp(lossPercent, 0., 100.) * 2.5;
	r = std::clamp(r, 0., 100.);
	const auto mos = 1. + 0.035 * r + 0.000007 * r * (r - 60.) * (100. - r);
	return std::clamp(mos, 1., 4.5);
}

MediaProblem CheckMedia() {
	if (!Ffi::Library::Present()) {
		return MediaProblem::LibraryMissing;
	}
	// Loaded once already and refused: say so before the next call starts.
	if (!Ffi::Library::LoadError().isEmpty()) {
		return MediaProblem::LibraryBroken;
	}
	return MediaProblem::None;
}

} // namespace Calls
