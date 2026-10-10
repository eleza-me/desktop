#include "search/search_debounce.h"

namespace Search {

Debouncer::Debouncer(int delayMs, QObject *parent)
: QObject(parent) {
	_timer.setSingleShot(true);
	_timer.setInterval(delayMs);
	connect(&_timer, &QTimer::timeout, this, [this] { fire(); });
}

void Debouncer::edited(const QString &text) {
	_pending = text.trimmed();
	if (_pending.isEmpty()) {
		// Back to the browse state without a delay.
		fire();
	} else if (_pending == _current && _firedOnce) {
		// Typed and erased a character: nothing to do.
		_timer.stop();
	} else {
		_timer.start();
	}
}

void Debouncer::flush() {
	if (_timer.isActive()) {
		fire();
	}
}

void Debouncer::set(const QString &text) {
	_pending = text.trimmed();
	fire();
}

void Debouncer::cancel() {
	_timer.stop();
	_pending = _current;
}

void Debouncer::fire() {
	_timer.stop();
	if (_firedOnce && _pending == _current) {
		return;
	}
	_firedOnce = true;
	_current = _pending;
	Q_EMIT fired(_current);
}

} // namespace Search
