// One running app per data directory: a second launch tells the first one to show
// its window (and passes its route, if any) and exits.
#pragma once

#include <QtCore/QObject>
#include <QtNetwork/QLocalServer>

namespace Core {

class SingleInstance final : public QObject {
	Q_OBJECT

public:
	explicit SingleInstance(const QString &workDir, QObject *parent = nullptr);

	// True: another instance is running and got our message — exit now.
	// False: we are the first, the server is listening.
	[[nodiscard]] bool notifyRunning(const QString &message);

Q_SIGNALS:
	// A later launch asked us to activate; `message` is what it sent.
	void activated(QString message);

private:
	void handleConnection();

	QString _name;
	QLocalServer _server;

};

} // namespace Core
