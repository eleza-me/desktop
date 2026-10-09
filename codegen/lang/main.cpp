// codegen_lang — compiles lang.strings (fa) + lang_en.strings to lang_auto.h/.cpp.
//
//   codegen_lang -o <out dir> lang.strings lang_en.strings
#include "lang/lang_parser.h"
#include "lang/lang_generator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

#include <cstdio>

namespace {

int Fail(const QString &text) {
	std::fputs(text.toUtf8().constData(), stderr);
	std::fputc('\n', stderr);
	return 1;
}

} // namespace

int main(int argc, char *argv[]) {
	using namespace codegen::lang;

	const auto app = QCoreApplication(argc, argv);
	const auto args = app.arguments();
	auto outputPath = QString();
	auto inputs = QStringList();
	for (auto i = 1; i < args.size(); ++i) {
		if (args[i] == QStringLiteral("-o") && i + 1 < args.size()) {
			outputPath = args[++i];
		} else if (!args[i].startsWith(QLatin1Char('-'))) {
			inputs.push_back(args[i]);
		} else {
			inputs.clear();
			break;
		}
	}
	if (outputPath.isEmpty() || inputs.size() != 2) {
		std::fputs(
			"usage: codegen_lang -o <output dir> <lang.strings> <lang_en.strings>\n",
			stderr);
		return 2;
	}

	auto error = Error();
	auto catalog = Catalog();
	if (!ParseFile(inputs[0], catalog.base, error)
		|| !ParseFile(inputs[1], catalog.other, error)
		|| !Validate(catalog, error)) {
		return Fail(error.format());
	}
	auto writeError = QString();
	if (!WriteFiles(outputPath, Generate(catalog), writeError)) {
		return Fail(QStringLiteral("codegen_lang: error: ") + writeError);
	}
	return 0;
}
