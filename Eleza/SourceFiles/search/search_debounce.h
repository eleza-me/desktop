// The search field's debounce: the query goes out a moment after the user
// stopped typing, at once on Enter and at once when the field was emptied.
// The same query is not sent twice in a row. QtCore only (unit tested).
//
//   Search::Debouncer _debounce{ 300 };
//   connect(field, &Ui::SearchField::changed, &_debounce, &Search::Debouncer::edited);
//   connect(field, &Ui::SearchField::submitted, &_debounce, &Search::Debouncer::flush);
//   connect(&_debounce, &Search::Debouncer::fired, this, [=](const QString &query) { ... });
#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>

namespace Search {

class Debouncer final : public QObject {
	Q_OBJECT

public:
	static constexpr auto kDefaultDelay = 300;

	explicit Debouncer(int delayMs = kDefaultDelay, QObject *parent = nullptr);

	// The text of the field changed.
	void edited(const QString &text);
	// Enter: whatever is pending goes out now.
	void flush();
	// Sets the query without waiting (a route with ?q=, a recent search).
	void set(const QString &text);
	// Forgets the pending edit.
	void cancel();

	// The last query that went out (trimmed).
	[[nodiscard]] const QString &current() const {
		return _current;
	}
	[[nodiscard]] bool pending() const {
		return _timer.isActive();
	}

Q_SIGNALS:
	void fired(const QString &query);

private:
	void fire();

	QTimer _timer;
	QString _pending;
	QString _current;
	bool _firedOnce = false;

};

} // namespace Search
