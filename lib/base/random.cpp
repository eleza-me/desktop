#include "base/random.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QUuid>

namespace base {

QByteArray RandomBytes(int size) {
	auto result = QByteArray(size, Qt::Uninitialized);
	auto generator = QRandomGenerator::system();
	auto data = reinterpret_cast<uchar*>(result.data());
	for (auto i = 0; i < size; ++i) {
		data[i] = uchar(generator->generate() & 0xFFU);
	}
	return result;
}

QString RandomUuid() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

double RandomUnit() {
	return QRandomGenerator::system()->generateDouble();
}

quint32 RandomBounded(quint32 max) {
	return max ? QRandomGenerator::system()->bounded(max) : 0;
}

} // namespace base
