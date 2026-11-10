/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gls/gls_glass.h"

#include "mainwidget.h"
#include "styles/style_window.h"
#include "ui/painter.h"
#include "window/section_widget.h"
#include "window/window_session_controller.h"

namespace Gls {
namespace {

constexpr auto kTintAlpha = 0.74;
constexpr auto kBorderAlpha = 0.10;
constexpr auto kHighlightAlpha = 0.22;
constexpr auto kMargin = 8;
constexpr auto kRadius = 16;

} // namespace

void PaintChatBackdrop(
		QPainter &p,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> widget,
		QRect clip) {
	const auto content = controller->content();
	const auto offset = content->mapFromGlobal(
		widget->mapToGlobal(QPoint())).y()
		+ content->backgroundFromY();
	p.save();
	p.translate(0, -offset);
	Window::SectionWidget::PaintBackground(
		p,
		controller->currentChatTheme(),
		QSize(widget->width(), content->height()),
		clip.translated(0, offset));
	p.restore();
}

void PaintGlassPanel(QPainter &p, QRect rect, int radius) {
	auto hq = PainterHighQualityEnabler(p);

	auto tint = st::windowBg->c;
	tint.setAlphaF(kTintAlpha);
	p.setPen(Qt::NoPen);
	p.setBrush(tint);
	p.drawRoundedRect(rect, radius, radius);

	auto border = QColor(255, 255, 255);
	border.setAlphaF(kBorderAlpha);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(border, 1.));
	const auto half = 0.5;
	const auto edge = QRectF(rect).marginsRemoved({ half, half, half, half });
	p.drawRoundedRect(edge, radius, radius);

	auto highlight = QLinearGradient(edge.topLeft(), edge.bottomLeft());
	auto top = QColor(255, 255, 255);
	top.setAlphaF(kHighlightAlpha);
	auto transparent = top;
	transparent.setAlphaF(0.);
	highlight.setColorAt(0., top);
	highlight.setColorAt(std::min(1., (radius * 1.5) / edge.height()), transparent);
	p.setPen(QPen(QBrush(highlight), 1.));
	p.drawRoundedRect(edge, radius, radius);
}

int GlassMargin() {
	return style::ConvertScale(kMargin);
}

int GlassRadius() {
	return style::ConvertScale(kRadius);
}

} // namespace Gls
