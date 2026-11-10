/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Gls {

inline constexpr auto kBarTintAlpha = 0.78;

// Paints the current chat wallpaper behind a widget so that it lines up
// with the wallpaper of the chat area, as if the widget were transparent.
void PaintChatBackdrop(
	QPainter &p,
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> widget,
	QRect clip);

// Frosted rounded panel: translucent tint, hairline border and a light
// highlight along the top edge.
void PaintGlassPanel(QPainter &p, QRect rect, int radius);

[[nodiscard]] int GlassMargin();
[[nodiscard]] int GlassRadius();

} // namespace Gls
