// Ui::object_ptr<T> — owns a QObject that may also be destroyed by its Qt parent.
// Deletes the object in the destructor if it is still alive.
//
//   Ui::object_ptr<Ui::FlatLabel>(parent, text)        creates new FlatLabel(parent, text)
//   Ui::Owned<Base>(new Derived(parent, ...))          wraps a pointer you already have
//   layout->add(std::move(ptr))                        gives the ownership away
#pragma once

#include <QtCore/QPointer>

#include <type_traits>
#include <utility>

namespace Ui {

template <typename Object>
class object_ptr;

namespace details {

template <typename Type>
struct is_object_ptr : std::false_type {
};
template <typename Type>
struct is_object_ptr<object_ptr<Type>> : std::true_type {
};

} // namespace details

template <typename Object>
class object_ptr {
public:
	object_ptr() = default;
	object_ptr(std::nullptr_t) {
	}

	// Always creates a new object: object_ptr<Label>(parent, text) is
	// new Label(parent, text). To wrap an existing pointer use Ui::Owned(ptr).
	template <
		typename First,
		typename ...Args,
		typename = std::enable_if_t<
			!details::is_object_ptr<std::decay_t<First>>::value
			&& !std::is_same_v<std::decay_t<First>, std::nullptr_t>
			&& std::is_constructible_v<Object, First, Args...>>>
	explicit object_ptr(First &&first, Args &&...args)
	: _object(new Object(
		std::forward<First>(first),
		std::forward<Args>(args)...)) {
	}

	// Takes the ownership of an existing object.
	[[nodiscard]] static object_ptr FromRaw(Object *value) {
		auto result = object_ptr();
		result._object = value;
		return result;
	}

	object_ptr(const object_ptr &other) = delete;
	object_ptr &operator=(const object_ptr &other) = delete;
	object_ptr(object_ptr &&other) noexcept
	: _object(std::exchange(other._object, nullptr)) {
	}
	object_ptr &operator=(object_ptr &&other) noexcept {
		if (this != &other) {
			destroy();
			_object = std::exchange(other._object, nullptr);
		}
		return *this;
	}

	template <
		typename Other,
		typename = std::enable_if_t<std::is_base_of_v<Object, Other>>>
	object_ptr(object_ptr<Other> &&other) noexcept
	: _object(other.release()) {
	}

	~object_ptr() {
		destroy();
	}

	[[nodiscard]] Object *data() const {
		return static_cast<Object*>(_object.data());
	}
	[[nodiscard]] Object *get() const {
		return data();
	}
	[[nodiscard]] Object *operator->() const {
		return data();
	}
	[[nodiscard]] Object &operator*() const {
		return *data();
	}
	operator Object*() const {
		return data();
	}
	explicit operator bool() const {
		return !_object.isNull();
	}

	// Gives the ownership away (to a Qt parent, usually).
	[[nodiscard]] Object *release() {
		return static_cast<Object*>(
			std::exchange(_object, nullptr).data());
	}

	void destroy() {
		delete release();
	}
	void destroyDelayed() {
		if (const auto value = release()) {
			if constexpr (std::is_base_of_v<QObject, Object>) {
				value->deleteLater();
			} else {
				delete value;
			}
		}
	}

	template <typename ...Args>
	Object *create(Args &&...args) {
		destroy();
		_object = new Object(std::forward<Args>(args)...);
		return data();
	}

private:
	QPointer<QObject> _object;

};

// Wraps an existing object: Ui::Owned<Window::SectionWidget>(new ChatSection(...)).
template <typename Object>
[[nodiscard]] inline object_ptr<Object> Owned(Object *value) {
	return object_ptr<Object>::FromRaw(value);
}

} // namespace Ui
