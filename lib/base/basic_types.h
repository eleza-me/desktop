// Common includes and aliases used across the non-GUI code.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

// Callback alias: Fn<void(int)>.
template <typename Signature>
using Fn = std::function<Signature>;

// u"text"_s (QString), "text"_L1 (QLatin1StringView), "text"_ba (QByteArray).
using namespace Qt::Literals::StringLiterals;

using TimeId = qint64; // unix time, seconds
using TimeMs = qint64; // unix time, milliseconds
