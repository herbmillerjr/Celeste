#include <QScrollBar>
#include "globals.h"
#include "widgets.h"

namespace StyleSheet
{
	const QString Colors(const QColor &foreground,const QColor &background)
	{
		return QString("color: rgba(%1,%2,%3,%4); background-color: rgba(%5,%6,%7,%8);").arg(
			StringConvert::Integer(foreground.red()),
			StringConvert::Integer(foreground.green()),
			StringConvert::Integer(foreground.blue()),
			StringConvert::Integer(foreground.alpha()),
			StringConvert::Integer(background.red()),
			StringConvert::Integer(background.green()),
			StringConvert::Integer(background.blue()),
			StringConvert::Integer(background.alpha())
		);
	}
}

PinnedTextEdit::PinnedTextEdit(QWidget *parent) : QTextEdit(parent)
{
	connect(this,&PinnedTextEdit::textChanged,[this]() {
		Tail();
	});
}

void PinnedTextEdit::resizeEvent(QResizeEvent *event)
{
	Tail();
	QTextEdit::resizeEvent(event);
}

void PinnedTextEdit::Tail()
{
	QTextCursor cursor=textCursor();
	cursor.movePosition(QTextCursor::End);
	setTextCursor(cursor);
	ensureCursorVisible();
}

void PinnedTextEdit::Append(const QString &text)
{
	Tail();
	if (!toPlainText().isEmpty()) insertPlainText("\n"); // FIXME: this is really inefficient
	insertHtml(text);
}

const int ScrollingTextEdit::PAUSE=3000;

ScrollingTextEdit::ScrollingTextEdit(QWidget *parent) : QTextEdit(parent)
{

}

void ScrollingTextEdit::showEvent(QShowEvent *event)
{
	scrollTimer.setInterval(50);
	QTimer::singleShot(PAUSE,[this]() {
		connect(&scrollTimer,&QTimer::timeout,[this]() {
			verticalScrollBar()->setSliderPosition(verticalScrollBar()->sliderPosition()+1);
		});
		connect(verticalScrollBar(),&QScrollBar::valueChanged,this,&ScrollingTextEdit::Scrolled);
		scrollTimer.start();
	});
}

void ScrollingTextEdit::Scrolled(int position)
{
	if (position >= verticalScrollBar()->maximum())
	{
		QTimer::singleShot(PAUSE,[this]() {
			emit Finished();
		});
	}
}