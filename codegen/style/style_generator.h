// Emits C++ for a parsed style module / palette.
//
//   styles/palette.h, styles/palette.cpp       namespace st { extern const style::color name; }
//   styles/style_<name>.h, style_<name>.cpp    namespace style { struct X {...}; }
//                                              namespace st { extern const T &name; }
#pragma once

#include "style/style_parser.h"

namespace codegen::style {

struct GeneratedFile {
	QString name; // file name, no directory
	QByteArray content;
};

[[nodiscard]] QList<GeneratedFile> GeneratePalette(const Palette &palette);
[[nodiscard]] QList<GeneratedFile> GenerateModule(const Module &module);

// Writes only when the content differs (keeps timestamps for incremental builds
// unless `touch` is set). Returns false and fills `error` on I/O failure.
bool WriteFiles(
	const QString &directory,
	const QList<GeneratedFile> &files,
	QString &error);

} // namespace codegen::style
