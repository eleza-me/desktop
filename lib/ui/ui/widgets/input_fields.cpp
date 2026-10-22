#include "ui/widgets/input_fields.h"

#include "base/text_direction.h"
#include "ui/emoji/emoji_pack.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/painter.h"
#include "ui/ui_phrases.h"
#include "ui/ui_utility.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QMetaMethod>
#include <QtCore/QMimeData>
#include <QtCore/QTimerEvent>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QSyntaxHighlighter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Ui {
namespace {

constexpr auto kCaretBlink = 530;
// An emoji against the text, as in Ui::Text::String.
constexpr auto kEmojiScale = 1.3;

[[nodiscard]] bool HasStrongDirection(const QString &text, bool *rtl) {
	const auto strong = base::FirstStrongIsRtl(text);
	if (strong) {
		*rtl = *strong;
	}
	return strong.has_value();
}

// The length the server checks: code points, a surrogate pair is one.
[[nodiscard]] int CodePoints(QStringView text) {
	auto result = 0;
	const auto size = text.size();
	for (auto i = qsizetype(0); i != size; ++i, ++result) {
		if (text[i].isHighSurrogate()
			&& i + 1 != size
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
	}
	return result;
}

// Moves `from` back by up to `count` code points, not below `limit`.
[[nodiscard]] int BackCodePoints(
		QStringView text,
		int from,
		int limit,
		int *count) {
	while (*count > 0 && from > limit) {
		--from;
		if (from > limit
			&& text[from].isLowSurrogate()
			&& text[from - 1].isHighSurrogate()) {
			--from;
		}
		--*count;
	}
	return from;
}

[[nodiscard]] QString ShortcutText(QKeySequence::StandardKey key) {
	return QKeySequence(key).toString(QKeySequence::NativeText);
}

void PaintPlaceholder(
		Painter &p,
		const QRect &rect,
		const style::InputField &st,
		const QString &text,
		bool startIsRight,
		double focused,
		double hidden) {
	if (text.isEmpty() || hidden >= 1. || rect.width() <= 0) {
		return;
	}
	const auto &font = st.placeholderFont;
	const auto full = font->width(text);
	const auto elided = (full > rect.width())
		? font->elided(text, rect.width())
		: text;
	const auto width = (full > rect.width()) ? font->width(elided) : full;
	const auto shift = int(std::lround(hidden * st.placeholderShift));
	const auto left = startIsRight
		? (rect.x() + rect.width() - width - shift)
		: (rect.x() + shift);
	p.save();
	p.setClipRect(rect);
	p.setOpacity(p.opacity() * (1. - hidden));
	p.setLayoutDirection(startIsRight ? Qt::RightToLeft : Qt::LeftToRight);
	p.setFont(font);
	p.setPen(anim::color(
		st.placeholderFg->c,
		st.placeholderFgActive->c,
		focused));
	p.drawText(left, rect.y() + font->ascent, elided);
	p.restore();
}

} // namespace

QString LatinizeDigits(const QString &text) {
	auto result = text;
	for (auto &ch : result) {
		const auto code = ch.unicode();
		if (code >= 0x06F0 && code <= 0x06F9) {
			ch = QChar(ushort('0' + (code - 0x06F0)));
		} else if (code >= 0x0660 && code <= 0x0669) {
			ch = QChar(ushort('0' + (code - 0x0660)));
		}
	}
	return result;
}

void PaintInputFrame(
		QPainter &p,
		const QRect &rect,
		const style::InputField &st,
		double focused,
		double error) {
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = std::min(double(st.radius), rect.height() / 2.);
	const auto border = st.border + (st.borderActive - st.border) * focused;
	if (st.textBg->c.alpha() > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(st.textBg);
		p.drawRoundedRect(rect, radius, radius);
	}
	if (border > 0.) {
		const auto base = anim::color(
			st.borderFg->c,
			st.borderFgActive->c,
			focused);
		const auto color = anim::color(base, st.borderFgError->c, error);
		if (color.alpha() > 0) {
			const auto half = border / 2.;
			p.setPen(QPen(color, border));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(rect).marginsRemoved(QMarginsF(half, half, half, half)),
				radius - half,
				radius - half);
		}
	}
}

namespace {

// Emoji of the pack in an editor, drawn as their pictures while the text keeps
// them (typing, undo, copy, the text() of the field are untouched): every emoji
// gets the width of its picture and no ink — its characters in a 1 px font with
// letter spacing, transparent — and the editor paints the pictures over them.
// Formats of a highlighter live in the layouts only, not in the document.
class EmojiHighlighter final : public QSyntaxHighlighter {
public:
	struct BlockData final : QTextBlockUserData {
		std::vector<Emoji::Range> ranges;
	};

	EmojiHighlighter(QTextDocument *document, const style::font &font)
	: QSyntaxHighlighter(document)
	, _font(font->f) {
		const auto pixels = (_font.pixelSize() > 0)
			? _font.pixelSize()
			: font->height;
		_size = std::max(
			std::min(int(std::lround(pixels * kEmojiScale)), font->height),
			1);
		_advance = _size + 2 * std::max(_size / 16, 1);
		_tiny = _font;
		_tiny.setPixelSize(1);
		_tiny.setKerning(false);
	}

	[[nodiscard]] int size() const {
		return _size;
	}
	[[nodiscard]] int advance() const {
		return _advance;
	}

protected:
	void highlightBlock(const QString &text) override {
		auto data = std::make_unique<BlockData>();
		data->ranges = Emoji::FindAll(text);
		for (const auto &range : data->ranges) {
			const auto length = range.till - range.from;
			setFormat(
				range.from,
				length,
				format(text.mid(range.from, length)));
		}
		setCurrentBlockUserData(data->ranges.empty() ? nullptr : data.release());
	}

private:
	// An emoji's characters take `_advance` in all: the 1 px font's own
	// advances and the letter spacing, which Qt adds once per cluster.
	[[nodiscard]] QTextCharFormat format(const QString &emoji) {
		auto i = _spacing.constFind(emoji);
		if (i == _spacing.cend()) {
			const auto natural = [&](qreal spacing) {
				auto font = _tiny;
				font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
				auto layout = QTextLayout(emoji, font);
				layout.beginLayout();
				auto line = layout.createLine();
				line.setLineWidth(1'000'000);
				layout.endLayout();
				return line.naturalTextWidth();
			};
			const auto base = natural(0.);
			const auto clusters = std::max(
				int(std::lround((natural(16.) - base) / 16.)),
				1);
			i = _spacing.insert(emoji, (_advance - base) / clusters);
		}
		auto font = _tiny;
		font.setLetterSpacing(QFont::AbsoluteSpacing, *i);
		auto result = QTextCharFormat();
		result.setFont(font, QTextCharFormat::FontPropertiesAll);
		result.setForeground(QBrush(Qt::transparent));
		return result;
	}

	QFont _font;
	QFont _tiny;
	int _size = 0;
	int _advance = 0;
	QHash<QString, qreal> _spacing;

};

} // namespace

class InputField::Inner final : public QTextEdit {
public:
	explicit Inner(InputField *owner) : QTextEdit(owner), _owner(owner) {
		if (Emoji::Available()) {
			_emoji = new EmojiHighlighter(document(), owner->_st.font);
		}
	}

	// The owner is being destroyed: the focus leaves this widget inside
	// ~QWidget of the owner, when there is no InputField to tell any more.
	void detach() {
		_owner = nullptr;
	}

protected:
	void focusInEvent(QFocusEvent *e) override {
		QTextEdit::focusInEvent(e);
		_owner->innerFocusChanged(true);
	}
	void focusOutEvent(QFocusEvent *e) override {
		QTextEdit::focusOutEvent(e);
		if (_owner) {
			_owner->innerFocusChanged(false);
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (!_owner->innerKeyPress(e)) {
			QTextEdit::keyPressEvent(e);
		}
	}
	void paintEvent(QPaintEvent *e) override {
		QTextEdit::paintEvent(e);
		if (_emoji) {
			paintEmoji(e->rect());
		}
	}
	void contextMenuEvent(QContextMenuEvent *e) override {
		_owner->innerContextMenu(e->globalPos());
	}
	bool canInsertFromMimeData(const QMimeData *source) const override {
		return source && source->hasText();
	}
	void insertFromMimeData(const QMimeData *source) override {
		if (!source || !source->hasText()) {
			return;
		}
		auto text = source->text();
		text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
		text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
		if (_owner->_mode == Mode::SingleLine) {
			text.replace(QLatin1Char('\n'), QLatin1Char(' '));
		}
		textCursor().insertText(text);
	}

private:
	void paintEmoji(QRect clip) {
		using Data = EmojiHighlighter::BlockData;
		auto p = QPainter(viewport());
		const auto offset = QPointF(
			-horizontalScrollBar()->value(),
			-verticalScrollBar()->value());
		const auto size = qreal(_emoji->size());
		const auto advance = _emoji->advance();
		const auto layout = document()->documentLayout();
		for (auto block = document()->firstBlock(); block.isValid(); block = block.next()) {
			const auto data = static_cast<const Data*>(block.userData());
			if (!data || !block.layout()) {
				continue;
			}
			const auto bounds = layout->blockBoundingRect(block).translated(offset);
			if (bounds.bottom() < clip.top()) {
				continue;
			} else if (bounds.top() > clip.bottom()) {
				break;
			}
			const auto text = block.layout();
			const auto origin = text->position() + offset;
			for (const auto &range : data->ranges) {
				const auto line = text->lineForTextPosition(range.from);
				if (!line.isValid()) {
					continue;
				}
				// The emoji's edges: from its start to its end when both are
				// on one run, else from its start and its first character's
				// direction.
				auto leading = range.from;
				auto trailing = range.from;
				const auto start = line.cursorToX(&leading, QTextLine::Leading);
				const auto first = line.cursorToX(&trailing, QTextLine::Trailing);
				const auto end = line.cursorToX(range.till);
				const auto left = (std::abs(std::abs(end - start) - advance) < 1.5)
					? std::min(start, end)
					: (first >= start)
					? start
					: (start - advance);
				Emoji::Paint(p, range.id, QRectF(
					origin.x() + left + (advance - size) / 2.,
					origin.y() + line.y() + (line.height() - size) / 2.,
					size,
					size));
			}
		}
	}

	InputField *_owner = nullptr;
	EmojiHighlighter *_emoji = nullptr;

};

InputField::InputField(
	QWidget *parent,
	const style::InputField &st,
	const QString &placeholder,
	const QString &value)
: InputField(parent, st, Mode::SingleLine, placeholder, value) {
}

InputField::InputField(
	QWidget *parent,
	const style::InputField &st,
	Mode mode,
	const QString &placeholder,
	const QString &value)
: RpWidget(parent)
, _st(st)
, _mode(mode)
, _inner(new Inner(this))
, _placeholder(placeholder) {
	_inner->setFrameStyle(QFrame::NoFrame);
	_inner->setAcceptRichText(false);
	_inner->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_inner->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_inner->setTabChangesFocus(true);
	_inner->setUndoRedoEnabled(true);
	_inner->viewport()->setAutoFillBackground(false);
	_inner->setAutoFillBackground(false);
	_inner->setContentsMargins(0, 0, 0, 0);
	_inner->document()->setDocumentMargin(0);
	_inner->setFont(_st.font->f);
	_inner->document()->setDefaultFont(_st.font->f);
	_inner->setCursorWidth(1);
	if (_mode == Mode::SingleLine) {
		_inner->setLineWrapMode(QTextEdit::NoWrap);
		_inner->setWordWrapMode(QTextOption::NoWrap);
	} else {
		_inner->setLineWrapMode(QTextEdit::WidgetWidth);
		_inner->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	}
	setFocusProxy(_inner);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(style::cur_text);
	refreshPalette();
	style::OnPaletteChanged(this, [this] { refreshPalette(); });

	QObject::connect(
		_inner->document(),
		&QTextDocument::contentsChanged,
		this,
		[this] { innerContentsChanged(); });
	QObject::connect(
		_inner->document(),
		&QTextDocument::contentsChange,
		this,
		[this](int position, int removed, int added) {
			Q_UNUSED(removed);
			_changeFrom = std::min(_changeFrom, position);
			_changeTill = std::max(_changeTill, position + added);
		});

	updateDirection();
	resize(width(), heightForContent());
	if (!value.isEmpty()) {
		setText(value);
	}
}

InputField::~InputField() {
	_inner->detach();
}

QString InputField::text() const {
	return _lastText;
}

void InputField::setText(const QString &text) {
	auto value = text;
	if (_mode == Mode::SingleLine) {
		value.replace(QLatin1Char('\n'), QLatin1Char(' '));
	}
	if (_maxLength > 0) {
		auto excess = CodePoints(value) - _maxLength;
		if (excess > 0) {
			value.truncate(
				BackCodePoints(value, int(value.size()), 0, &excess));
		}
	}
	_inner->setPlainText(value);
	setCursorToEnd();
}

void InputField::clear() {
	_inner->clear();
}

bool InputField::empty() const {
	return _lastText.isEmpty();
}

void InputField::insertText(const QString &text) {
	_inner->textCursor().insertText(text);
}

void InputField::setPlaceholder(const QString &placeholder) {
	_placeholder = placeholder;
	update();
}

void InputField::setMaxLength(int length) {
	_maxLength = length;
	innerContentsChanged();
}

void InputField::setSubmitSettings(SubmitSettings settings) {
	_submitSettings = settings;
}

void InputField::setForcedDirection(
		std::optional<Qt::LayoutDirection> direction) {
	_forcedDirection = direction;
	updateDirection();
	update();
}

void InputField::setReadOnly(bool readOnly) {
	_inner->setReadOnly(readOnly);
}

void InputField::setFocusFast() {
	_inner->setFocus();
}

bool InputField::hasFocus() const {
	return _inner->hasFocus();
}

void InputField::selectAll() {
	_inner->selectAll();
}

void InputField::setCursorToEnd() {
	auto cursor = _inner->textCursor();
	cursor.movePosition(QTextCursor::End);
	_inner->setTextCursor(cursor);
}

void InputField::showError() {
	if (!_error) {
		_error = true;
		_errorAnimation.start(
			[this] { update(); },
			0.,
			1.,
			_st.duration,
			anim::linear);
		update();
	}
	setFocusFast();
}

void InputField::hideError() {
	if (_error) {
		_error = false;
		_errorAnimation.start(
			[this] { update(); },
			1.,
			0.,
			_st.duration,
			anim::linear);
		update();
	}
}

QTextEdit *InputField::rawTextEdit() const {
	return _inner;
}

void InputField::refreshPalette() {
	auto palette = _inner->palette();
	palette.setColor(QPalette::Text, _st.textFg->c);
	palette.setColor(QPalette::Base, Qt::transparent);
	palette.setColor(QPalette::Window, Qt::transparent);
	palette.setColor(QPalette::Highlight, st::selectionBg->c);
	palette.setColor(QPalette::HighlightedText, _st.textFg->c);
	_inner->setPalette(palette);
	_inner->viewport()->setPalette(palette);
	update();
}

void InputField::updateDirection() {
	auto rtl = style::RightToLeft();
	auto state = 0;
	if (_forcedDirection) {
		state = (*_forcedDirection == Qt::RightToLeft) ? 1 : 0;
	} else if (HasStrongDirection(_lastText, &rtl)) {
		// Single-line: the whole field follows the first strong character.
		// Multi-line: every paragraph has its own direction.
		state = (_mode == Mode::MultiLine) ? 2 : (rtl ? 1 : 0);
	} else {
		state = style::RightToLeft() ? 1 : 0;
	}
	if (_directionState == state) {
		return;
	}
	_directionState = state;
	auto option = _inner->document()->defaultTextOption();
	if (state == 2) {
		option.setTextDirection(Qt::LayoutDirectionAuto);
		option.setAlignment(Qt::AlignLeft);
	} else if (state == 1) {
		option.setTextDirection(Qt::RightToLeft);
		option.setAlignment(Qt::AlignRight | Qt::AlignAbsolute);
	} else {
		option.setTextDirection(Qt::LeftToRight);
		option.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
	}
	_correcting = true;
	_inner->document()->setDefaultTextOption(option);
	_correcting = false;
}

int InputField::contentHeight() const {
	const auto size = _inner->document()->documentLayout()->documentSize();
	return std::max(int(std::ceil(size.height())), _st.font->height);
}

int InputField::heightForContent() const {
	if (_mode == Mode::SingleLine) {
		return _st.heightMin;
	}
	return std::clamp(
		contentHeight() + _st.textMargins.top() + _st.textMargins.bottom(),
		_st.heightMin,
		std::max(_st.heightMax, _st.heightMin));
}

void InputField::updateInnerGeometry() {
	const auto margins = style::rtlmargins(_st.textMargins);
	const auto innerWidth = std::max(
		width() - margins.left() - margins.right(),
		1);
	if (_mode == Mode::SingleLine) {
		const auto lineHeight = contentHeight();
		_inner->setGeometry(
			margins.left(),
			(height() - lineHeight) / 2,
			innerWidth,
			lineHeight);
	} else {
		const auto top = margins.top();
		const auto available = height() - margins.top() - margins.bottom();
		const auto content = contentHeight();
		// Center a short text in a taller frame (one line in heightMin).
		const auto innerHeight = std::min(content, std::max(available, 1));
		_inner->setGeometry(
			margins.left(),
			top + std::max((available - innerHeight) / 2, 0),
			innerWidth,
			innerHeight);
	}
}

int InputField::resizeGetHeight(int newWidth) {
	if (_mode == Mode::MultiLine) {
		const auto innerWidth = std::max(
			newWidth - _st.textMargins.left() - _st.textMargins.right(),
			1);
		_correcting = true;
		_inner->document()->setTextWidth(innerWidth);
		_correcting = false;
	}
	return heightForContent();
}

void InputField::innerFocusChanged(bool focused) {
	if (_focused == focused) {
		return;
	}
	_focused = focused;
	_focusAnimation.start(
		[this] { update(); },
		focused ? 0. : 1.,
		focused ? 1. : 0.,
		_st.duration,
		anim::linear);
	update();
	emit focusedChanged(focused);
}

void InputField::innerContentsChanged() {
	if (_correcting) {
		return;
	}
	auto text = _inner->toPlainText();
	auto excess = (_maxLength > 0) ? (CodePoints(text) - _maxLength) : 0;
	if (excess > 0) {
		// The excess goes from the end of what was just typed or pasted (the
		// caret stays after the part that fits), never splitting a surrogate
		// pair; whatever is still over (a lowered limit) from the end.
		_correcting = true;
		const auto size = int(text.size());
		auto till = std::clamp(_changeTill, 0, size);
		const auto from = std::clamp(_changeFrom, 0, till);
		if (till > 0
			&& till < size
			&& text[till].isLowSurrogate()
			&& text[till - 1].isHighSurrogate()) {
			++till;
		}
		const auto remove = [&](int start, int end) {
			if (start < end) {
				auto cursor = QTextCursor(_inner->document());
				cursor.setPosition(start);
				cursor.setPosition(end, QTextCursor::KeepAnchor);
				cursor.removeSelectedText();
			}
		};
		remove(BackCodePoints(text, till, from, &excess), till);
		if (excess > 0) {
			text = _inner->toPlainText();
			const auto end = int(text.size());
			remove(BackCodePoints(text, end, 0, &excess), end);
		}
		_correcting = false;
		text = _inner->toPlainText();
	}
	_changeFrom = std::numeric_limits<int>::max();
	_changeTill = -1;
	const auto changedText = (text != _lastText);
	_lastText = text;
	const auto hasText = !text.isEmpty();
	if (_hasText != hasText) {
		_hasText = hasText;
		_placeholderAnimation.start(
			[this] { update(); },
			hasText ? 0. : 1.,
			hasText ? 1. : 0.,
			_st.duration,
			anim::easeOutCubic);
	}
	updateDirection();
	const auto wanted = heightForContent();
	if (wanted != height()) {
		resize(width(), wanted);
	} else {
		updateInnerGeometry();
	}
	update();
	if (changedText) {
		hideError();
		emit changed();
	}
}

bool InputField::innerKeyPress(QKeyEvent *e) {
	const auto key = e->key();
	const auto modifiers = e->modifiers();
	if (key == Qt::Key_Escape) {
		// Whoever listens to cancelled() handles the key. Without a listener
		// it travels on to the parents (a box, the window's "back"); never
		// both, or one press would go back twice.
		static const auto signal = QMetaMethod::fromSignal(
			&InputField::cancelled);
		if (isSignalConnected(signal)) {
			e->accept();
			emit cancelled();
		} else {
			e->ignore();
		}
		return true;
	} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		const auto ctrl = (modifiers & (Qt::ControlModifier | Qt::MetaModifier))
			!= 0;
		const auto shift = (modifiers & Qt::ShiftModifier) != 0;
		auto submit = false;
		if (_mode == Mode::SingleLine) {
			submit = (_submitSettings != SubmitSettings::None);
			if (!submit) {
				return true;
			}
		} else {
			switch (_submitSettings) {
			case SubmitSettings::Enter: submit = !shift; break;
			case SubmitSettings::CtrlEnter: submit = ctrl; break;
			case SubmitSettings::Both: submit = !shift; break;
			case SubmitSettings::None: submit = false; break;
			}
		}
		if (submit) {
			emit submitted(modifiers);
		} else if (!_inner->isReadOnly()) {
			_inner->textCursor().insertText(QStringLiteral("\n"));
			_inner->ensureCursorVisible();
		}
		return true;
	} else if (_mode == Mode::SingleLine
		&& (key == Qt::Key_Tab || key == Qt::Key_Backtab)) {
		emit tabbed(key == Qt::Key_Backtab);
		return false;
	}
	return false;
}

void InputField::innerContextMenu(const QPoint &globalPosition) {
	const auto &phrases = CurrentPhrases();
	const auto inner = _inner;
	const auto document = inner->document();
	const auto hasSelection = inner->textCursor().hasSelection();
	const auto readOnly = inner->isReadOnly();
	const auto clipboard = QGuiApplication::clipboard()->mimeData();
	const auto canPaste = !readOnly && clipboard && clipboard->hasText();

	_contextMenu = new PopupMenu(this);
	const auto menu = _contextMenu.data();
	const auto add = [&](
			const QString &text,
			std::function<void()> callback,
			bool enabled,
			QKeySequence::StandardKey key) {
		auto item = MenuItem();
		item.text = text;
		item.callback = std::move(callback);
		item.disabled = !enabled;
		item.shortcut = ShortcutText(key);
		menu->addItem(std::move(item));
	};
	if (!readOnly) {
		add(
			phrases.undo,
			[=] { inner->undo(); },
			document->isUndoAvailable(),
			QKeySequence::Undo);
		add(
			phrases.redo,
			[=] { inner->redo(); },
			document->isRedoAvailable(),
			QKeySequence::Redo);
		menu->addSeparator();
		add(
			phrases.cut,
			[=] { inner->cut(); },
			hasSelection,
			QKeySequence::Cut);
	}
	add(phrases.copy, [=] { inner->copy(); }, hasSelection, QKeySequence::Copy);
	if (!readOnly) {
		add(
			phrases.paste,
			[=] { inner->paste(); },
			canPaste,
			QKeySequence::Paste);
		add(
			phrases.remove,
			[=] { inner->textCursor().removeSelectedText(); },
			hasSelection,
			QKeySequence::Delete);
	}
	menu->addSeparator();
	add(
		phrases.selectAll,
		[=] { inner->selectAll(); },
		!_lastText.isEmpty(),
		QKeySequence::SelectAll);
	if (_contextMenuHook) {
		_contextMenuHook(menu);
	}
	menu->popup(globalPosition);
}

void InputField::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto focused = _focusAnimation.value(_focused ? 1. : 0.);
	const auto error = _errorAnimation.value(_error ? 1. : 0.);
	PaintInputFrame(p, rect(), _st, focused, error);

	const auto hidden = _placeholderAnimation.value(_hasText ? 1. : 0.);
	const auto margins = style::rtlmargins(_st.textMargins);
	const auto area = QRect(
		margins.left(),
		(_mode == Mode::SingleLine)
			? ((height() - _st.placeholderFont->height) / 2)
			: (_inner->y()
				+ (_st.font->height - _st.placeholderFont->height) / 2),
		width() - margins.left() - margins.right(),
		_st.placeholderFont->height);
	const auto startIsRight = _forcedDirection
		? (*_forcedDirection == Qt::RightToLeft)
		: style::RightToLeft();
	PaintPlaceholder(p, area, _st, _placeholder, startIsRight, focused, hidden);
}

void InputField::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateInnerGeometry();
}

void InputField::mousePressEvent(QMouseEvent *e) {
	Q_UNUSED(e);
	_inner->setFocus();
}

void InputField::focusInEvent(QFocusEvent *e) {
	RpWidget::focusInEvent(e);
	_inner->setFocus();
}

// A smaller bullet for hidden text than the platform's default.
class LineEditStyle final : public QProxyStyle {
public:
	int styleHint(
			StyleHint hint,
			const QStyleOption *option,
			const QWidget *widget,
			QStyleHintReturn *returnData) const override {
		if (hint == SH_LineEdit_PasswordCharacter) {
			return 0x2022;
		}
		return QProxyStyle::styleHint(hint, option, widget, returnData);
	}

};

class MaskedInputField::Inner final : public QLineEdit {
public:
	explicit Inner(MaskedInputField *owner) : QLineEdit(owner), _owner(owner) {
		const auto style = new LineEditStyle();
		style->setParent(this);
		setStyle(style);
	}

	// See InputField::Inner::detach().
	void detach() {
		_owner = nullptr;
	}

protected:
	void focusInEvent(QFocusEvent *e) override {
		QLineEdit::focusInEvent(e);
		_owner->innerFocusChanged(true);
	}
	void focusOutEvent(QFocusEvent *e) override {
		QLineEdit::focusOutEvent(e);
		if (_owner) {
			_owner->innerFocusChanged(false);
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (!_owner->innerKeyPress(e)) {
			QLineEdit::keyPressEvent(e);
		}
	}
	void contextMenuEvent(QContextMenuEvent *e) override {
		_owner->innerContextMenu(e->globalPos());
	}

private:
	MaskedInputField *_owner = nullptr;

};

MaskedInputField::MaskedInputField(
	QWidget *parent,
	const style::InputField &st,
	const QString &placeholder,
	const QString &value)
: RpWidget(parent)
, _st(st)
, _inner(new Inner(this))
, _placeholder(placeholder) {
	_inner->setFrame(false);
	_inner->setAutoFillBackground(false);
	_inner->setAttribute(Qt::WA_MacShowFocusRect, false);
	_inner->setTextMargins(0, 0, 0, 0);
	_inner->setContentsMargins(0, 0, 0, 0);
	_inner->setFont(_st.font->f);
	_inner->setAlignment(Qt::AlignLeading | Qt::AlignVCenter);
	setFocusProxy(_inner);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(style::cur_text);
	refreshPalette();
	style::OnPaletteChanged(this, [this] { refreshPalette(); });

	QObject::connect(_inner, &QLineEdit::textChanged, this, [this] {
		innerTextChanged();
	});
	QObject::connect(
		_inner,
		&QLineEdit::cursorPositionChanged,
		this,
		[this](int, int now) {
			if (!_correcting) {
				_lastCursor = now;
			}
		});
	updateDirection();
	resize(width(), _st.heightMin);
	if (!value.isEmpty()) {
		setText(value);
	}
}

MaskedInputField::~MaskedInputField() {
	_inner->detach();
}

QString MaskedInputField::text() const {
	return _lastText;
}

void MaskedInputField::setText(const QString &text) {
	_inner->setText(text);
}

void MaskedInputField::clear() {
	_inner->clear();
}

void MaskedInputField::setPlaceholder(const QString &placeholder) {
	_placeholder = placeholder;
	update();
}

void MaskedInputField::setMaxLength(int length) {
	_inner->setMaxLength((length > 0) ? length : 32767);
}

void MaskedInputField::setEchoMode(QLineEdit::EchoMode mode) {
	_inner->setEchoMode(mode);
}

void MaskedInputField::setForcedDirection(
		std::optional<Qt::LayoutDirection> direction) {
	_forcedDirection = direction;
	updateDirection();
	update();
}

void MaskedInputField::setTrailingPadding(int padding) {
	_trailingPadding = padding;
	updateInnerGeometry();
}

void MaskedInputField::setReadOnly(bool readOnly) {
	_inner->setReadOnly(readOnly);
}

void MaskedInputField::setFocusFast() {
	_inner->setFocus();
}

bool MaskedInputField::hasFocus() const {
	return _inner->hasFocus();
}

void MaskedInputField::selectAll() {
	_inner->selectAll();
}

void MaskedInputField::setCursorPosition(int position) {
	_inner->setCursorPosition(position);
}

int MaskedInputField::cursorPosition() const {
	return _inner->cursorPosition();
}

void MaskedInputField::showError() {
	if (!_error) {
		_error = true;
		_errorAnimation.start(
			[this] { update(); },
			0.,
			1.,
			_st.duration,
			anim::linear);
		update();
	}
	setFocusFast();
}

void MaskedInputField::hideError() {
	if (_error) {
		_error = false;
		_errorAnimation.start(
			[this] { update(); },
			1.,
			0.,
			_st.duration,
			anim::linear);
		update();
	}
}

QLineEdit *MaskedInputField::rawLineEdit() const {
	return _inner;
}

void MaskedInputField::refreshPalette() {
	auto palette = _inner->palette();
	palette.setColor(QPalette::Text, _st.textFg->c);
	palette.setColor(QPalette::Base, Qt::transparent);
	palette.setColor(QPalette::Window, Qt::transparent);
	palette.setColor(QPalette::Highlight, st::selectionBg->c);
	palette.setColor(QPalette::HighlightedText, _st.textFg->c);
	_inner->setPalette(palette);
	update();
}

void MaskedInputField::updateDirection() {
	auto rtl = style::RightToLeft();
	if (_forcedDirection) {
		rtl = (*_forcedDirection == Qt::RightToLeft);
	} else {
		auto detected = false;
		if (HasStrongDirection(_lastText, &detected)) {
			rtl = detected;
		}
	}
	const auto direction = rtl ? Qt::RightToLeft : Qt::LeftToRight;
	if (_inner->layoutDirection() != direction) {
		_inner->setLayoutDirection(direction);
	}
}

void MaskedInputField::updateInnerGeometry() {
	auto margins = _st.textMargins;
	margins.setRight(margins.right() + _trailingPadding);
	margins = style::rtlmargins(margins);
	_inner->setGeometry(
		margins.left(),
		0,
		std::max(width() - margins.left() - margins.right(), 1),
		height());
}

int MaskedInputField::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.heightMin;
}

void MaskedInputField::innerFocusChanged(bool focused) {
	if (_focused == focused) {
		return;
	}
	_focused = focused;
	_focusAnimation.start(
		[this] { update(); },
		focused ? 0. : 1.,
		focused ? 1. : 0.,
		_st.duration,
		anim::linear);
	update();
	emit focusedChanged(focused);
}

void MaskedInputField::innerTextChanged() {
	if (_correcting) {
		return;
	}
	const auto original = _inner->text();
	const auto originalCursor = _inner->cursorPosition();
	auto now = original;
	auto cursor = originalCursor;
	correctValue(_lastText, _lastCursor, now, cursor);
	if (now != original || cursor != originalCursor) {
		_correcting = true;
		if (now != original) {
			_inner->setText(now);
		}
		_inner->setCursorPosition(std::clamp(cursor, 0, int(now.size())));
		_correcting = false;
	}
	_lastCursor = _inner->cursorPosition();
	if (now == _lastText) {
		return;
	}
	const auto hadText = !_lastText.isEmpty();
	_lastText = now;
	const auto hasText = !now.isEmpty();
	if (hadText != hasText) {
		_placeholderAnimation.start(
			[this] { update(); },
			hasText ? 0. : 1.,
			hasText ? 1. : 0.,
			_st.duration,
			anim::easeOutCubic);
	}
	updateDirection();
	hideError();
	update();
	emit changed();
}

bool MaskedInputField::innerKeyPress(QKeyEvent *e) {
	const auto key = e->key();
	if (key == Qt::Key_Escape) {
		// The same rule as InputField: a listener handles it, or the parents.
		static const auto signal = QMetaMethod::fromSignal(
			&MaskedInputField::cancelled);
		if (isSignalConnected(signal)) {
			e->accept();
			emit cancelled();
		} else {
			e->ignore();
		}
		return true;
	} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		emit submitted(e->modifiers());
		return true;
	} else if (key == Qt::Key_Backspace
		&& _inner->cursorPosition() == 0
		&& !_inner->hasSelectedText()) {
		emit backspaceAtStart();
		return true;
	}
	return false;
}

void MaskedInputField::innerContextMenu(const QPoint &globalPosition) {
	const auto &phrases = CurrentPhrases();
	const auto inner = _inner;
	const auto hasSelection = inner->hasSelectedText();
	const auto readOnly = inner->isReadOnly();
	const auto hidden = (inner->echoMode() != QLineEdit::Normal);
	const auto clipboard = QGuiApplication::clipboard()->mimeData();
	const auto canPaste = !readOnly && clipboard && clipboard->hasText();

	_contextMenu = new PopupMenu(this);
	const auto menu = _contextMenu.data();
	const auto add = [&](
			const QString &text,
			std::function<void()> callback,
			bool enabled,
			QKeySequence::StandardKey key) {
		auto item = MenuItem();
		item.text = text;
		item.callback = std::move(callback);
		item.disabled = !enabled;
		item.shortcut = ShortcutText(key);
		menu->addItem(std::move(item));
	};
	if (!readOnly) {
		add(
			phrases.undo,
			[=] { inner->undo(); },
			inner->isUndoAvailable(),
			QKeySequence::Undo);
		add(
			phrases.redo,
			[=] { inner->redo(); },
			inner->isRedoAvailable(),
			QKeySequence::Redo);
		menu->addSeparator();
		add(
			phrases.cut,
			[=] { inner->cut(); },
			hasSelection && !hidden,
			QKeySequence::Cut);
	}
	add(
		phrases.copy,
		[=] { inner->copy(); },
		hasSelection && !hidden,
		QKeySequence::Copy);
	if (!readOnly) {
		add(
			phrases.paste,
			[=] { inner->paste(); },
			canPaste,
			QKeySequence::Paste);
		add(
			phrases.remove,
			[=] { inner->del(); },
			hasSelection,
			QKeySequence::Delete);
	}
	menu->addSeparator();
	add(
		phrases.selectAll,
		[=] { inner->selectAll(); },
		!_lastText.isEmpty(),
		QKeySequence::SelectAll);
	menu->popup(globalPosition);
}

void MaskedInputField::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto focused = _focusAnimation.value(_focused ? 1. : 0.);
	const auto error = _errorAnimation.value(_error ? 1. : 0.);
	PaintInputFrame(p, rect(), _st, focused, error);

	const auto hidden = _placeholderAnimation.value(
		_lastText.isEmpty() ? 0. : 1.);
	const auto area = QRect(
		_inner->x(),
		(height() - _st.placeholderFont->height) / 2,
		_inner->width(),
		_st.placeholderFont->height);
	const auto startIsRight = _forcedDirection
		? (*_forcedDirection == Qt::RightToLeft)
		: style::RightToLeft();
	PaintPlaceholder(p, area, _st, _placeholder, startIsRight, focused, hidden);
}

void MaskedInputField::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateInnerGeometry();
}

void MaskedInputField::mousePressEvent(QMouseEvent *e) {
	Q_UNUSED(e);
	_inner->setFocus();
}

void MaskedInputField::focusInEvent(QFocusEvent *e) {
	RpWidget::focusInEvent(e);
	_inner->setFocus();
}

DigitsInput::DigitsInput(
	QWidget *parent,
	const style::InputField &st,
	const QString &placeholder,
	const QString &value,
	int maxDigits)
: MaskedInputField(parent, st, placeholder)
, _maxDigits(maxDigits) {
	setForcedDirection(Qt::LeftToRight);
	rawLineEdit()->setInputMethodHints(Qt::ImhDigitsOnly);
	if (!value.isEmpty()) {
		setText(value);
	}
}

void DigitsInput::correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) {
	Q_UNUSED(was);
	Q_UNUSED(wasCursor);
	const auto latin = LatinizeDigits(now);
	auto result = QString();
	result.reserve(latin.size());
	auto cursor = nowCursor;
	for (auto i = 0; i != latin.size(); ++i) {
		const auto ch = latin[i];
		const auto keep = ch.unicode() >= '0'
			&& ch.unicode() <= '9'
			&& (_maxDigits <= 0 || result.size() < _maxDigits);
		if (keep) {
			result.append(ch);
		} else if (i < nowCursor) {
			--cursor;
		}
	}
	now = result;
	nowCursor = std::clamp(cursor, 0, int(result.size()));
}

PasswordInput::PasswordInput(
	QWidget *parent,
	const style::InputField &st,
	const QString &placeholder)
: MaskedInputField(parent, st, placeholder)
, _toggle(new IconButton(this, st::passwordToggleButton)) {
	setEchoMode(QLineEdit::Password);
	setTrailingPadding(st::passwordToggleButton.width);
	_toggle->setClickedCallback([this] {
		setRevealed(!_revealed);
	});
	_toggle->setFocusPolicy(Qt::NoFocus);
}

void PasswordInput::setRevealed(bool revealed) {
	_revealed = revealed;
	setEchoMode(revealed ? QLineEdit::Normal : QLineEdit::Password);
	_toggle->setIconOverride(revealed ? &st::passwordHideIcon : nullptr);
}

void PasswordInput::resizeEvent(QResizeEvent *e) {
	MaskedInputField::resizeEvent(e);
	_toggle->moveToRight(
		st::spaceXs,
		(height() - _toggle->height()) / 2,
		width());
}

class PhoneInput::CodePart final : public MaskedInputField {
public:
	using MaskedInputField::MaskedInputField;

	std::function<void(QString overflow)> overflowed;
	std::function<void()> wantsNext;

protected:
	void correctValue(
			const QString &was,
			int wasCursor,
			QString &now,
			int &nowCursor) override {
		Q_UNUSED(was);
		Q_UNUSED(wasCursor);
		const auto latin = LatinizeDigits(now);
		auto digits = QString();
		for (const auto ch : latin) {
			if (ch.unicode() >= '0' && ch.unicode() <= '9') {
				digits.append(ch);
			}
		}
		const auto limit = 4;
		if (digits.size() > limit) {
			const auto overflow = digits.mid(limit);
			digits.truncate(limit);
			if (overflowed) {
				const auto callback = overflowed;
				PostponeCall(this, [=] { callback(overflow); });
			}
		}
		now = digits;
		nowCursor = std::min(nowCursor, int(digits.size()));
	}

};

class PhoneInput::NumberPart final : public MaskedInputField {
public:
	using MaskedInputField::MaskedInputField;

	// A whole number in the international form ("+98 912…", "0098912…") was
	// pasted or typed: its digits without the prefix.
	std::function<void(QString digits)> international;

protected:
	void correctValue(
			const QString &was,
			int wasCursor,
			QString &now,
			int &nowCursor) override {
		Q_UNUSED(was);
		Q_UNUSED(wasCursor);
		const auto latin = LatinizeDigits(now);
		const auto trimmed = latin.trimmed();
		const auto plus = trimmed.startsWith(QLatin1Char('+'));
		if (international
			&& (plus || trimmed.startsWith(QStringLiteral("00")))) {
			auto all = QString();
			for (const auto ch : trimmed.mid(plus ? 1 : 2)) {
				if (ch.unicode() >= '0' && ch.unicode() <= '9') {
					all.append(ch);
				}
			}
			if (all.size() >= 5) {
				// The owner splits it into the code and the number.
				const auto callback = international;
				PostponeCall(this, [=] { callback(all); });
			}
		}
		auto digits = QString();
		auto cursor = nowCursor;
		for (auto i = 0; i != latin.size(); ++i) {
			const auto ch = latin[i];
			if (ch.unicode() >= '0' && ch.unicode() <= '9' && digits.size() < 15) {
				digits.append(ch);
			} else if (i < nowCursor) {
				--cursor;
			}
		}
		now = digits;
		nowCursor = std::clamp(cursor, 0, int(digits.size()));
	}

};

PhoneInput::PhoneInput(
	QWidget *parent,
	const style::InputField &st,
	const QString &countryCode)
: RpWidget(parent)
, _st(st)
, _code(new CodePart(this, st::phonePartField))
, _number(new NumberPart(this, st::phonePartField)) {
	_code->setForcedDirection(Qt::LeftToRight);
	_number->setForcedDirection(Qt::LeftToRight);
	_code->rawLineEdit()->setInputMethodHints(Qt::ImhDialableCharactersOnly);
	_number->rawLineEdit()->setInputMethodHints(Qt::ImhDialableCharactersOnly);
	_code->setText(countryCode);
	setCursor(style::cur_text);
	resize(width(), _st.heightMin);

	_code->overflowed = [this](QString overflow) {
		_number->setText(overflow + _number->text());
		_number->setFocusFast();
		_number->setCursorPosition(int(overflow.size()));
	};
	_number->international = [this](QString digits) {
		emit internationalNumberEntered(digits);
	};
	const auto watchFocus = [this](bool) {
		setFocused(_code->hasFocus() || _number->hasFocus());
	};
	QObject::connect(_code, &MaskedInputField::focusedChanged, this, watchFocus);
	QObject::connect(_number, &MaskedInputField::focusedChanged, this, watchFocus);
	QObject::connect(_code, &MaskedInputField::changed, this, [this] {
		if (_error) {
			_error = false;
			update();
		}
		emit countryCodeChanged(_code->text());
		emit changed();
	});
	QObject::connect(_number, &MaskedInputField::changed, this, [this] {
		if (_error) {
			_error = false;
			update();
		}
		emit changed();
	});
	QObject::connect(_code, &MaskedInputField::submitted, this, [this] {
		_number->setFocusFast();
	});
	QObject::connect(_number, &MaskedInputField::submitted, this, [this] {
		emit submitted();
	});
	QObject::connect(_code, &MaskedInputField::cancelled, this, [this] {
		emit cancelled();
	});
	QObject::connect(_number, &MaskedInputField::cancelled, this, [this] {
		emit cancelled();
	});
	QObject::connect(_number, &MaskedInputField::backspaceAtStart, this, [this] {
		_code->setFocusFast();
		auto code = _code->text();
		code.chop(1);
		_code->setText(code);
		_code->setCursorPosition(int(code.size()));
	});
	setFocusProxy(_number);
}

PhoneInput::~PhoneInput() {
	// The fields are children: they lose the focus inside ~QWidget, when the
	// handlers above would run on what is left of this object.
	QObject::disconnect(_code, nullptr, this, nullptr);
	QObject::disconnect(_number, nullptr, this, nullptr);
}

void PhoneInput::setCountryCode(const QString &code) {
	_code->setText(code);
}

QString PhoneInput::countryCode() const {
	return _code->text();
}

void PhoneInput::setNumber(const QString &number) {
	_number->setText(number);
}

QString PhoneInput::number() const {
	return _number->text();
}

QString PhoneInput::fullNumber() const {
	auto number = _number->text();
	while (number.startsWith(QLatin1Char('0'))) {
		number.remove(0, 1);
	}
	if (number.isEmpty() || _code->text().isEmpty()) {
		return QString();
	}
	return QLatin1Char('+') + _code->text() + number;
}

void PhoneInput::setNumberPlaceholder(const QString &placeholder) {
	_number->setPlaceholder(placeholder);
}

void PhoneInput::setFocusFast() {
	_number->setFocusFast();
}

void PhoneInput::showError() {
	_error = true;
	_errorAnimation.start(
		[this] { update(); },
		0.,
		1.,
		_st.duration,
		anim::linear);
	_number->setFocusFast();
	update();
}

void PhoneInput::setFocused(bool focused) {
	if (_focused == focused) {
		return;
	}
	_focused = focused;
	_focusAnimation.start(
		[this] { update(); },
		focused ? 0. : 1.,
		focused ? 1. : 0.,
		_st.duration,
		anim::linear);
	update();
}

int PhoneInput::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.heightMin;
}

void PhoneInput::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	// Phone numbers are always written left-to-right.
	const auto left = _st.textMargins.left();
	const auto plusWidth = _st.font->width(QLatin1Char('+'));
	const auto codeWidth = _st.font->width(QStringLiteral("0000")) + st::spaceXs;
	_code->setGeometry(left + plusWidth, 0, codeWidth, height());
	const auto numberLeft = left + plusWidth + codeWidth + 2 * st::spaceSm + 1;
	_number->setGeometry(
		numberLeft,
		0,
		std::max(width() - numberLeft - _st.textMargins.right(), 1),
		height());
}

void PhoneInput::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto focused = _focusAnimation.value(_focused ? 1. : 0.);
	const auto error = _errorAnimation.value(_error ? 1. : 0.);
	PaintInputFrame(p, rect(), _st, focused, error);
	p.setFont(_st.font);
	p.setPen(_st.placeholderFg);
	p.drawText(
		_st.textMargins.left(),
		(height() - _st.font->height) / 2 + _st.font->ascent,
		QStringLiteral("+"));
	const auto lineLeft = _code->x() + _code->width() + st::spaceSm;
	p.fillRect(
		lineLeft,
		st::spaceMd,
		st::lineWidth,
		height() - 2 * st::spaceMd,
		_st.borderFg);
}

void PhoneInput::mousePressEvent(QMouseEvent *e) {
	if (e->pos().x() < _code->x() + _code->width() + st::spaceSm) {
		_code->setFocusFast();
	} else {
		_number->setFocusFast();
	}
}

CodeInput::CodeInput(QWidget *parent, int digits, const style::CodeInput &st)
: RpWidget(parent)
, _st(st)
, _count(std::max(digits, 1)) {
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_InputMethodEnabled);
	setInputMethodHints(Qt::ImhDigitsOnly);
	setCursor(style::cur_text);
	resize(naturalWidth(), _st.height);
}

void CodeInput::setDigitsCount(int digits) {
	_count = std::max(digits, 1);
	_digits.truncate(_count);
	_cursor = std::min(_cursor, _count - 1);
	resize(naturalWidth(), _st.height);
	update();
}

QString CodeInput::code() const {
	return _digits;
}

void CodeInput::setCode(const QString &code) {
	_digits.clear();
	_cursor = 0;
	insertDigits(code);
}

void CodeInput::clear() {
	if (!_digits.isEmpty()) {
		_digits.clear();
		_cursor = 0;
		changedNotify();
	}
	update();
}

void CodeInput::showError() {
	_error = true;
	_errorAnimation.start(
		[this] { update(); },
		0.,
		1.,
		st::motionFast,
		anim::linear);
	_shakeAnimation.start(
		[this] { update(); },
		0.,
		1.,
		st::motionSlow,
		anim::linear);
	update();
}

void CodeInput::setDisabled(bool disabled) {
	_disabled = disabled;
	update();
}

int CodeInput::naturalWidth() const {
	return _count * _st.width + (_count - 1) * _st.skip;
}

int CodeInput::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

QRect CodeInput::boxRect(int index) const {
	// Codes are read left-to-right in both languages.
	const auto left = (width() - naturalWidth()) / 2;
	return QRect(
		left + index * (_st.width + _st.skip),
		0,
		_st.width,
		_st.height);
}

void CodeInput::changedNotify() {
	update();
	emit codeChanged(_digits);
	if (_digits.size() == _count) {
		emit finished(_digits);
	}
}

void CodeInput::insertDigits(const QString &text) {
	const auto latin = LatinizeDigits(text);
	auto changed = false;
	for (const auto ch : latin) {
		if (ch.unicode() < '0' || ch.unicode() > '9') {
			continue;
		}
		if (_cursor < _digits.size()) {
			_digits[_cursor] = ch;
		} else if (_digits.size() < _count) {
			_digits.append(ch);
		} else {
			break;
		}
		changed = true;
		if (_cursor < _count - 1) {
			++_cursor;
		} else {
			break;
		}
	}
	if (changed) {
		if (_error) {
			_error = false;
		}
		changedNotify();
	}
}

void CodeInput::paste() {
	const auto text = QGuiApplication::clipboard()->text();
	auto digits = QString();
	for (const auto ch : LatinizeDigits(text)) {
		if (ch.unicode() >= '0' && ch.unicode() <= '9') {
			digits.append(ch);
		}
	}
	if (digits.isEmpty()) {
		return;
	}
	if (digits.size() >= _count) {
		// A whole code replaces what was typed.
		_digits.clear();
		_cursor = 0;
	}
	insertDigits(digits);
}

void CodeInput::keyPressEvent(QKeyEvent *e) {
	if (_disabled) {
		e->ignore();
		return;
	}
	const auto key = e->key();
	if (e->matches(QKeySequence::Paste)) {
		paste();
	} else if (key == Qt::Key_Backspace) {
		if (_cursor < _digits.size() && _cursor == _count - 1) {
			_digits.remove(_cursor, 1);
		} else if (_cursor > 0) {
			--_cursor;
			if (_cursor < _digits.size()) {
				_digits.remove(_cursor, 1);
			}
		} else if (!_digits.isEmpty()) {
			_digits.remove(0, 1);
		} else {
			return;
		}
		_error = false;
		changedNotify();
	} else if (key == Qt::Key_Delete) {
		if (_cursor < _digits.size()) {
			_digits.remove(_cursor, 1);
			_error = false;
			changedNotify();
		}
	} else if (key == Qt::Key_Left) {
		_cursor = std::max(_cursor - 1, 0);
		update();
	} else if (key == Qt::Key_Right) {
		_cursor = std::min(_cursor + 1, std::min(int(_digits.size()), _count - 1));
		update();
	} else if (key == Qt::Key_Home) {
		_cursor = 0;
		update();
	} else if (key == Qt::Key_End) {
		_cursor = std::min(int(_digits.size()), _count - 1);
		update();
	} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		if (_digits.size() == _count) {
			emit finished(_digits);
		}
	} else if (key == Qt::Key_Escape) {
		static const auto signal = QMetaMethod::fromSignal(
			&CodeInput::cancelled);
		if (isSignalConnected(signal)) {
			e->accept();
			emit cancelled();
		} else {
			e->ignore();
		}
	} else if (!e->text().isEmpty()
		&& !(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
		insertDigits(e->text());
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void CodeInput::inputMethodEvent(QInputMethodEvent *e) {
	if (!_disabled && !e->commitString().isEmpty()) {
		insertDigits(e->commitString());
	}
	e->accept();
}

QVariant CodeInput::inputMethodQuery(Qt::InputMethodQuery query) const {
	switch (query) {
	case Qt::ImEnabled: return true;
	case Qt::ImHints: return int(Qt::ImhDigitsOnly);
	case Qt::ImCursorRectangle: return boxRect(_cursor);
	default: return RpWidget::inputMethodQuery(query);
	}
}

void CodeInput::mousePressEvent(QMouseEvent *e) {
	setFocus();
	for (auto i = 0; i != _count; ++i) {
		if (boxRect(i).marginsAdded(
			QMargins(_st.skip / 2, 0, _st.skip / 2, 0)).contains(e->pos())) {
			_cursor = std::min(i, std::min(int(_digits.size()), _count - 1));
			update();
			break;
		}
	}
}

void CodeInput::focusInEvent(QFocusEvent *e) {
	_caretShown = true;
	_caretTimer.start(kCaretBlink, this);
	update();
	RpWidget::focusInEvent(e);
}

void CodeInput::focusOutEvent(QFocusEvent *e) {
	_caretTimer.stop();
	_caretShown = false;
	update();
	RpWidget::focusOutEvent(e);
}

void CodeInput::timerEvent(QTimerEvent *e) {
	if (e->timerId() == _caretTimer.timerId()) {
		_caretShown = !_caretShown;
		update(boxRect(_cursor));
	} else {
		RpWidget::timerEvent(e);
	}
}

void CodeInput::contextMenuEvent(QContextMenuEvent *e) {
	const auto menu = new PopupMenu(this);
	auto item = MenuItem();
	item.text = CurrentPhrases().paste;
	item.callback = [this] { paste(); };
	item.disabled = QGuiApplication::clipboard()->text().isEmpty();
	item.shortcut = ShortcutText(QKeySequence::Paste);
	menu->addItem(std::move(item));
	menu->popup(e->globalPos());
}

void CodeInput::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto shake = _shakeAnimation.value(1.);
	const auto shift = (shake < 1.)
		? int(std::lround(
			std::sin(shake * 4. * 3.14159265358979) * 6. * (1. - shake)))
		: 0;
	const auto error = _errorAnimation.value(_error ? 1. : 0.);
	const auto focused = hasFocus() && !_disabled;
	if (_disabled) {
		p.setOpacity(0.6);
	}
	const auto format = CurrentPhrases().formatNumber;
	for (auto i = 0; i != _count; ++i) {
		const auto rect = boxRect(i).translated(shift, 0);
		const auto active = focused && (i == _cursor);
		const auto border = active ? _st.borderActive : _st.border;
		const auto base = active ? _st.borderFgActive->c : _st.borderFg->c;
		const auto color = anim::color(base, _st.borderFgError->c, error);
		const auto half = border / 2.;
		p.setPen(QPen(color, border));
		p.setBrush(_st.bg);
		p.drawRoundedRect(
			QRectF(rect).marginsRemoved(QMarginsF(half, half, half, half)),
			_st.radius - half,
			_st.radius - half);
		if (i < _digits.size()) {
			const auto latin = QString(_digits[i]);
			const auto shown = format ? format(latin) : latin;
			p.setFont(_st.font);
			p.setPen(_st.textFg);
			const auto width = _st.font->width(shown);
			p.drawText(
				rect.x() + (rect.width() - width) / 2,
				rect.y() + (rect.height() - _st.font->height) / 2
					+ _st.font->ascent,
				shown);
		} else if (active && _caretShown) {
			const auto caretHeight = _st.font->height * 3 / 4;
			p.fillRect(
				QRectF(
					rect.x() + rect.width() / 2. - 0.75,
					rect.y() + (rect.height() - caretHeight) / 2.,
					1.5,
					caretHeight),
				_st.borderFgActive);
		}
	}
}

SearchField::SearchField(
	QWidget *parent,
	const QString &placeholder,
	const style::SearchField &st)
: RpWidget(parent)
, _st(st)
, _field(new InputField(this, st.field, placeholder))
, _clear(new FadeWrap<IconButton>(
	this,
	object_ptr<IconButton>(this, st.clear))) {
	_clear->hide(anim::type::instant);
	_clear->entity()->setFocusPolicy(Qt::NoFocus);
	_clear->entity()->setClickedCallback([this] {
		_field->clear();
		_field->setFocusFast();
	});
	QObject::connect(_field, &InputField::changed, this, [this] {
		const auto text = _field->text();
		_clear->toggle(!text.isEmpty(), anim::type::normal);
		emit changed(text);
	});
	QObject::connect(_field, &InputField::submitted, this, [this] {
		emit submitted();
	});
	QObject::connect(_field, &InputField::cancelled, this, [this] {
		static const auto signal = QMetaMethod::fromSignal(
			&SearchField::cancelled);
		if (!_field->empty()) {
			_field->clear();
		} else if (isSignalConnected(signal)) {
			emit cancelled();
		} else if (const auto parent = parentWidget()) {
			// Nobody handles an Esc in the empty field here: it goes on to
			// the parents as if the field had not seen it.
			auto event = QKeyEvent(
				QEvent::KeyPress,
				Qt::Key_Escape,
				Qt::NoModifier);
			QCoreApplication::sendEvent(parent, &event);
		}
	});
	QObject::connect(_field, &InputField::focusedChanged, this, [this] {
		update();
	});
	setFocusProxy(_field);
	setCursor(style::cur_text);
	resize(width(), _st.height);
}

SearchField::~SearchField() {
	// As in ~PhoneInput: no signals of the field after this point.
	QObject::disconnect(_field, nullptr, this, nullptr);
}

QString SearchField::text() const {
	return _field->text();
}

void SearchField::setText(const QString &text) {
	_field->setText(text);
}

void SearchField::clear() {
	_field->clear();
}

void SearchField::setFocusFast() {
	_field->setFocusFast();
}

int SearchField::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

void SearchField::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto left = _st.iconLeft + _st.icon.width() + _st.iconSkip;
	const auto right = _st.clear.width;
	_field->resize(std::max(width() - left - right, 1), _field->height());
	_field->moveToLeft(left, (height() - _field->height()) / 2, width());
	_clear->moveToRight(0, (height() - _clear->height()) / 2, width());
}

void SearchField::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(_st.bg);
		const auto radius = std::min(double(_st.radius), height() / 2.);
		p.drawRoundedRect(rect(), radius, radius);
	}
	_st.icon.paint(
		p,
		_st.iconLeft,
		(height() - _st.icon.height()) / 2,
		width());
}

void SearchField::mousePressEvent(QMouseEvent *e) {
	Q_UNUSED(e);
	_field->setFocusFast();
}

} // namespace Ui
