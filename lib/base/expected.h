// A tiny value-or-error holder (std::expected is C++23).
#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace base {

template <typename Error>
struct Unexpected {
	Error value;
};

template <typename Error>
[[nodiscard]] Unexpected<std::decay_t<Error>> make_unexpected(Error &&error) {
	return { std::forward<Error>(error) };
}

template <typename Value, typename Error>
class Expected {
public:
	Expected(Value value) : _data(std::in_place_index<0>, std::move(value)) {
	}
	Expected(Unexpected<Error> error) : _data(std::in_place_index<1>, std::move(error.value)) {
	}

	[[nodiscard]] bool has_value() const {
		return _data.index() == 0;
	}
	explicit operator bool() const {
		return has_value();
	}
	[[nodiscard]] Value &value() {
		return std::get<0>(_data);
	}
	[[nodiscard]] const Value &value() const {
		return std::get<0>(_data);
	}
	[[nodiscard]] Value &operator*() {
		return value();
	}
	[[nodiscard]] const Value &operator*() const {
		return value();
	}
	[[nodiscard]] Value *operator->() {
		return &value();
	}
	[[nodiscard]] const Value *operator->() const {
		return &value();
	}
	[[nodiscard]] const Error &error() const {
		return std::get<1>(_data);
	}

private:
	std::variant<Value, Error> _data;

};

} // namespace base
