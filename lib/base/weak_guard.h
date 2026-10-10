// Lifetime guards for asynchronous callbacks.
//
//   base::guard(this, [=](int value) { ... })   -> a callable that does nothing once `this` died
//   base::Guard                                  -> "alive?" token stored next to a callback
//
// Every asynchronous API in the core takes a guard object (or wraps its callbacks with
// base::guard) so a reply that arrives after its owner was destroyed is dropped silently.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>

namespace base {

// An optional QObject whose lifetime bounds a callback. A default-constructed Guard is
// "unbound" and always alive.
class Guard {
public:
	Guard() = default;
	Guard(QObject *object) : _object(object), _bound(object != nullptr) {
	}

	[[nodiscard]] bool alive() const {
		return !_bound || !_object.isNull();
	}
	[[nodiscard]] bool bound() const {
		return _bound;
	}
	[[nodiscard]] QObject *object() const {
		return _object.data();
	}

private:
	QPointer<QObject> _object;
	bool _bound = false;

};

// Wraps `callback` so it is invoked only while `object` is alive.
template <typename Callback>
[[nodiscard]] auto guard(QObject *object, Callback &&callback) {
	return [weak = QPointer<QObject>(object), callback = std::forward<Callback>(callback)](
			auto &&...args) mutable {
		if (!weak.isNull()) {
			callback(std::forward<decltype(args)>(args)...);
		}
	};
}

// Wraps `callback` so it is invoked only while `guard` is alive.
template <typename Callback>
[[nodiscard]] auto guard(Guard guard, Callback &&callback) {
	return [guard = std::move(guard), callback = std::forward<Callback>(callback)](
			auto &&...args) mutable {
		if (guard.alive()) {
			callback(std::forward<decltype(args)>(args)...);
		}
	};
}

// A token for classes that are not QObjects: `alive.make()` hands out weak
// flags that become false when the owner is destroyed.
class AliveFlag {
public:
	AliveFlag() : _flag(std::make_shared<bool>(true)) {
	}
	~AliveFlag() {
		*_flag = false;
	}
	AliveFlag(const AliveFlag &) = delete;
	AliveFlag &operator=(const AliveFlag &) = delete;

	[[nodiscard]] std::shared_ptr<const bool> make() const {
		return _flag;
	}

private:
	std::shared_ptr<bool> _flag;

};

} // namespace base
