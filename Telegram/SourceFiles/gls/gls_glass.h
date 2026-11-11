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

inline constexpr auto kDefaultTint = 58;
inline constexpr auto kMinTint = 20;
inline constexpr auto kMaxTint = 100;
inline constexpr auto kDefaultBlur = 8;
inline constexpr auto kMaxBlur = 20;
inline constexpr auto kDefaultRadius = 16;
inline constexpr auto kMaxRadius = 28;

// Appearance settings, stored with the other app options
[[nodiscard]] bool GlassEnabled();
[[nodiscard]] int GlassTint();
[[nodiscard]] int GlassBlur();
[[nodiscard]] int GlassRadiusSetting();
[[nodiscard]] bool GlassGaps();
void SetGlassEnabled(bool enabled);
void SetGlassTint(int tint);
void SetGlassBlur(int blur);
void SetGlassRadius(int radius);
void SetGlassGaps(bool gaps);
[[nodiscard]] rpl::producer<> SettingsChanges();

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
// a hairline border and a light highlight along the top edge. Each `slot`
// keeps its own cache, so a widget can paint several shapes
void PaintGlass(
	QPainter &p,
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> widget,
	QRect rect,
	int radius,
	int slot = 0);

// Called after the wallpaper is painted, fires `BackdropChanges` when
// the wallpaper image differs from the one the panels show. Changes of
// the appearance settings are reported there as well.
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

// Applies the Telegram GLS defaults once: folder tabs above the chat list
// and the light theme instead of the classic one
void ApplyDefaultsOnce();

// A bar across the chat: the wallpaper and a glass pill with gaps around it
void PaintBar(
	QPainter &p,
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> widget,
	QRect rect);

} // namespace Gls
