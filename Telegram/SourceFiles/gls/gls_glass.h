/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class ChatTheme;
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Gls {

// The chat wallpaper spans the whole window body: the chat area, the gaps
// between panels and the frosted panels all show one continuous image
[[nodiscard]] QSize WallpaperSize(
	not_null<Window::SessionController*> controller);

// Position of `widget` in the wallpaper, or `std::nullopt` when the widget
// lives outside the window body
[[nodiscard]] std::optional<QPoint> WallpaperOrigin(
	not_null<Window::SessionController*> controller,
	not_null<const QWidget*> widget);

void PaintWallpaper(
	QPainter &p,
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> widget,
	QRect clip);

// Frosted panel: blurred wallpaper under a translucent tint, a soft sheen,
// a hairline border and a light highlight along the top edge
void PaintGlass(
	QPainter &p,
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> widget,
	QRect rect,
	int radius);

// Called after the wallpaper is painted, fires `BackdropChanges` when
// the wallpaper image differs from the one the panels show
void TrackBackdrop(not_null<Ui::ChatTheme*> theme, QSize fill);
[[nodiscard]] rpl::producer<> BackdropChanges();

// A floating panel is inset from the window edges and rounded: the
// wallpaper is painted over its corners, above the panel children
void SetFloatingPanel(
	not_null<Window::SessionController*> controller,
	not_null<Ui::RpWidget*> panel,
	bool floating);
[[nodiscard]] int PanelRadius(not_null<const QWidget*> panel);

// Rounded selection and hover highlight of a chat list row
void PaintRowBackground(
	QPainter &p,
	QRect rect,
	bool active,
	bool selected);

[[nodiscard]] int GlassMargin();
[[nodiscard]] int GlassRadius();
[[nodiscard]] int BarRadius();
[[nodiscard]] int ComposeSkip();

} // namespace Gls
