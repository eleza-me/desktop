// codegen_style — compiles `.style` files and `colors.palette` to C++.
//
//   codegen_style --palette colors.palette --palette-only -o <out dir>
//   codegen_style --palette colors.palette [-I <dir>]... -o <out dir> file.style
#include "style/style_parser.h"
#include "style/style_generator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>

#include <cstdio>

namespace {

int Usage() {
	std::fputs(
		"usage: codegen_style --palette <colors.palette> [--palette-only]\n"
		"                     [-I <include dir>]... -o <output dir> [file.style]\n",
		stderr);
	return 2;
}

int Fail(const QString &text) {
	std::fputs(text.toUtf8().constData(), stderr);
	std::fputc('\n', stderr);
	return 1;
}

} // namespace

int main(int argc, char *argv[]) {
	using namespace codegen::style;

	const auto app = QCoreApplication(argc, argv);
	const auto args = app.arguments();

	auto options = Options();
	auto palettePath = QString();
	auto outputPath = QString();
	auto inputPath = QString();
	auto paletteOnly = false;
	for (auto i = 1; i < args.size(); ++i) {
		const auto &arg = args[i];
		const auto takeNext = [&](QString &to) {
			if (i + 1 >= args.size()) {
				return false;
			}
			to = args[++i];
			return true;
		};
		if (arg == QStringLiteral("--palette")) {
			if (!takeNext(palettePath)) {
				return Usage();
			}
		} else if (arg == QStringLiteral("--palette-only")) {
			paletteOnly = true;
		} else if (arg == QStringLiteral("-o")) {
			if (!takeNext(outputPath)) {
				return Usage();
			}
		} else if (arg == QStringLiteral("-I")) {
			auto dir = QString();
			if (!takeNext(dir)) {
				return Usage();
			}
			options.includePaths.push_back(dir);
		} else if (arg.startsWith(QStringLiteral("-I")) && arg.size() > 2) {
			options.includePaths.push_back(arg.mid(2));
		} else if (arg.startsWith(QLatin1Char('-'))) {
			return Usage();
		} else if (inputPath.isEmpty()) {
			inputPath = arg;
		} else {
			return Usage();
		}
	}
	if (palettePath.isEmpty()
		|| outputPath.isEmpty()
		|| (paletteOnly != inputPath.isEmpty())) {
		return Usage();
	}

	auto error = Error();
	auto palette = Palette();
	if (!ParsePalette(palettePath, palette, error)) {
		return Fail(error.format());
	}
	auto files = QList<GeneratedFile>();
	if (paletteOnly) {
		files = GeneratePalette(palette);
	} else {
		const auto module = ParseModule(inputPath, options, palette, error);
		if (!module) {
			return Fail(error.format());
		}
		files = GenerateModule(*module);
	}
	auto writeError = QString();
	if (!WriteFiles(outputPath, files, writeError)) {
		return Fail(QStringLiteral("codegen_style: error: ") + writeError);
	}
	return 0;
}
