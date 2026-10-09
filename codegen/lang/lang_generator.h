// Emits lang_auto.h / lang_auto.cpp for a validated catalog:
//
//   namespace Lang { enum class Key : ushort { lng_..., }; }
//   enum lngtag_user { lt_user };  enum lngtag_count { lt_count };   (global)
//   namespace tr {
//     QString lng_simple(now_t);
//     QString lng_with_tag(now_t, lngtag_user, const QString &user);
//     QString lng_plural(now_t, lngtag_count, double count);
//   }
#pragma once

#include "lang/lang_parser.h"

#include <QtCore/QByteArray>

namespace codegen::lang {

struct GeneratedFile {
	QString name;
	QByteArray content;
};

[[nodiscard]] QList<GeneratedFile> Generate(const Catalog &catalog);

bool WriteFiles(
	const QString &directory,
	const QList<GeneratedFile> &files,
	QString &error);

} // namespace codegen::lang
