/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gls/gls_glass.h"

#include "base/event_filter.h"
#include "base/options.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "mainwindow.h"
#include "ui/chat/chat_theme.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "window/section_widget.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_window.h"

#include <QtCore/QTimer>

namespace Gls {
namespace {

constexpr auto kDownscale = 6;
constexpr auto kSheenAlpha = 0.07;
constexpr auto kBorderAlpha = 0.12;
constexpr auto kHighlightAlpha = 0.30;
constexpr auto kRowOverAlpha = 0.08;
constexpr auto kMargin = 8;
constexpr auto kRowInset = 6;
constexpr auto kRowRadius = 10;
constexpr auto kMaxTrackedBackdrops = 16;
constexpr auto kBackdropFadeDuration = 250;

const auto kCornerName = QString("glsCorner");
const auto kFloatingProperty = "glsFloating";

const char kOptionGlass[] = "gls-glass";
const char kOptionGlassTint[] = "gls-glass-tint";
const char kOptionGlassBlur[] = "gls-glass-blur";
const char kOptionGlassRadius[] = "gls-glass-radius";
const char kOptionGlassGaps[] = "gls-glass-gaps";
const char kOptionDayThemeApplied[] = "gls-day-theme-applied";
const char kOptionLayoutApplied[] = "gls-layout-applied";
const auto kDayThemePath = u":/gui/day-blue.tdesktop-theme"_q;

base::options::toggle OptionGlass({
	.id = kOptionGlass,
	.name = "Liquid glass",
	.description = "Frosted translucent panels over the chat wallpaper.",
	.defaultValue = true,
});

base::options::option<int> OptionGlassTint({
	.id = kOptionGlassTint,
	.name = "Liquid glass tint",
	.description = "Opacity of the glass tint, in percent.",
	.defaultValue = kDefaultTint,
});

base::options::option<int> OptionGlassBlur({
	.id = kOptionGlassBlur,
	.name = "Liquid glass blur",
	.description = "Blur radius of the wallpaper under the glass.",
	.defaultValue = kDefaultBlur,
});

base::options::option<int> OptionGlassRadius({
	.id = kOptionGlassRadius,
	.name = "Liquid glass corners",
	.description = "Corner radius of the glass panels, in pixels.",
	.defaultValue = kDefaultRadius,
});

base::options::toggle OptionGlassGaps({
	.id = kOptionGlassGaps,
	.name = "Liquid glass gaps",
	.description = "Float the panels with gaps between them.",
	.defaultValue = true,
});

base::options::toggle OptionLayoutApplied({
	.id = kOptionLayoutApplied,
	.name = "Telegram GLS layout",
	.description = "The folder tabs of Telegram GLS were set up once.",
});

base::options::toggle OptionDayThemeApplied({
	.id = kOptionDayThemeApplied,
	.name = "Telegram GLS day theme",
	.description = "The light theme of Telegram GLS was applied once.",
});

struct BlurredWallpaper {
	const Ui::ChatTheme *theme = nullptr;
	qint64 key = 0;
	QSize fill;
	int blur = -1;
	QImage image;
	int serial = 0;
};

struct PanelCache {
	QImage image;
	QSize size;
	QPoint position;
	QRgb tint = 0;
	qint64 settings = 0;
	int radius = 0;
	int ratio = 0;
	int serial = -1;
};

[[nodiscard]] rpl::event_stream<> &BackdropChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] qint64 BackgroundKey(
		not_null<Ui::ChatTheme*> theme,
		const Ui::BackgroundState &state) {
	if (const auto color = theme->background().colorForFill) {
		return qint64(color->rgba()) | (qint64(1) << 40);
	}
	return state.now.waitingForNegativePattern
		? -1
		: state.now.pixmap.cacheKey();
}

[[nodiscard]] const BlurredWallpaper &Blurred(
		not_null<Ui::ChatTheme*> theme,
		QSize fill) {
	static auto result = BlurredWallpaper();

	const auto key = BackgroundKey(theme, theme->backgroundState(fill));
	const auto blur = GlassBlur();
	if (result.theme == theme
		&& result.key == key
		&& result.fill == fill
		&& result.blur == blur
		&& !result.image.isNull()) {
		return result;
	}
	const auto small = QSize(
		std::max(fill.width() / kDownscale, 1),
		std::max(fill.height() / kDownscale, 1));
	auto image = QImage(small, QImage::Format_ARGB32_Premultiplied);
	image.fill(st::windowBg->c);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.scale(
			small.width() / float64(fill.width()),
			small.height() / float64(fill.height()));
		Window::SectionWidget::PaintBackground(
			p,
			theme,
			fill,
			QRect(QPoint(), fill),
			true);
	}
	result.theme = theme;
	result.key = key;
	result.fill = fill;
	result.blur = blur;
	result.image = Images::BlurLargeImage(std::move(image), blur);
	++result.serial;
	return result;
}

[[nodiscard]] qint64 SettingsKey() {
	return (qint64(GlassEnabled() ? 1 : 0) << 32)
		| (qint64(GlassTint()) << 16)
		| qint64(GlassBlur());
}

using PanelKey = std::pair<const QWidget*, int>;

[[nodiscard]] std::map<PanelKey, PanelCache> &PanelCaches() {
	static auto result = std::map<PanelKey, PanelCache>();
	return result;
}

[[nodiscard]] PanelCache &PanelCacheFor(not_null<QWidget*> widget, int slot) {
	auto &caches = PanelCaches();
	const auto raw = widget.get();
	const auto key = PanelKey(raw, slot);
	const auto i = caches.find(key);
	if (i != end(caches)) {
		return i->second;
	}
	const auto known = caches.lower_bound(PanelKey(raw, std::numeric_limits<int>::min()));
	if (known == end(caches) || known->first.first != raw) {
		QObject::connect(raw, &QObject::destroyed, [=] {
			auto &caches = PanelCaches();
			caches.erase(
				caches.lower_bound(PanelKey(raw, std::numeric_limits<int>::min())),
				caches.upper_bound(PanelKey(raw, std::numeric_limits<int>::max())));
		});
	}
	return caches.emplace(key, PanelCache()).first->second;
}

void RenderPanel(
		PanelCache &cache,
		const BlurredWallpaper &blurred,
		QSize size,
		QPoint position,
		int radius) {
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);

	const auto full = QRectF(QPointF(), QSizeF(size));
	const auto enabled = GlassEnabled();
	auto q = QPainter(&image);
	q.setRenderHint(QPainter::SmoothPixmapTransform);

	auto white = QColor(255, 255, 255);
	auto clear = white;
	clear.setAlphaF(0.);
	if (enabled) {
		const auto sx = blurred.image.width() / float64(blurred.fill.width());
		const auto sy = blurred.image.height()
			/ float64(blurred.fill.height());
		q.drawImage(full, blurred.image, QRectF(
			position.x() * sx,
			position.y() * sy,
			size.width() * sx,
			size.height() * sy));

		auto tint = st::windowBg->c;
		tint.setAlphaF(GlassTint() / 100.);
		q.fillRect(full, tint);

		auto sheenColor = white;
		sheenColor.setAlphaF(kSheenAlpha);
		const auto diagonal = std::min(size.width(), size.height());
		auto sheen = QLinearGradient(
			QPointF(),
			QPointF(diagonal, diagonal));
		sheen.setColorAt(0., sheenColor);
		sheen.setColorAt(1., clear);
		q.fillRect(full, sheen);
	} else {
		q.fillRect(full, st::windowBg);
	}

	auto hq = PainterHighQualityEnabler(q);
	if (radius > 0) {
		auto outside = QPainterPath();
		outside.setFillRule(Qt::OddEvenFill);
		outside.addRect(full);
		outside.addRoundedRect(full, radius, radius);
		q.setCompositionMode(QPainter::CompositionMode_DestinationOut);
		q.setPen(Qt::NoPen);
		q.setBrush(Qt::black);
		q.drawPath(outside);
		q.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}
	if (radius > 0 && enabled) {
		const auto half = 0.5;
		const auto edge = full.marginsRemoved({ half, half, half, half });
		auto border = white;
		border.setAlphaF(kBorderAlpha);
		q.setBrush(Qt::NoBrush);
		q.setPen(QPen(border, 1.));
		q.drawRoundedRect(edge, radius, radius);

		auto top = white;
		top.setAlphaF(kHighlightAlpha);
		auto highlight = QLinearGradient(edge.topLeft(), edge.bottomLeft());
		highlight.setColorAt(0., top);
		highlight.setColorAt(
			std::min(1., (radius * 1.5) / edge.height()),
			clear);
		q.setPen(QPen(QBrush(highlight), 1.));
		q.drawRoundedRect(edge, radius, radius);
	}
	q.end();

	cache.image = std::move(image);
	cache.size = size;
	cache.position = position;
	cache.tint = st::windowBg->c.rgba();
	cache.settings = SettingsKey();
	cache.radius = radius;
	cache.ratio = ratio;
	cache.serial = blurred.serial;
}

void PaintCorner(
		QPainter &p,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> corner,
		QPoint center) {
	const auto size = corner->size();
	if (size.isEmpty()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	{
		auto q = QPainter(&image);
		PaintWallpaper(q, controller, corner, QRect(QPoint(), size));
		auto hq = PainterHighQualityEnabler(q);
		q.setCompositionMode(QPainter::CompositionMode_DestinationOut);
		q.setPen(Qt::NoPen);
		q.setBrush(Qt::black);
		q.drawEllipse(QPointF(center), size.width(), size.height());
	}
	p.drawImage(0, 0, image);
}

} // namespace

QSize WallpaperSize(not_null<Window::SessionController*> controller) {
	const auto body = controller->widget()->bodyWidget();
	return body ? body->size() : QSize();
}

std::optional<QPoint> WallpaperOrigin(
		not_null<Window::SessionController*> controller,
		not_null<const QWidget*> widget) {
	const auto body = controller->widget()->bodyWidget();
	if (!body) {
		return std::nullopt;
	} else if (body == widget.get()) {
		return QPoint();
	} else if (!body->isAncestorOf(widget)) {
		return std::nullopt;
	}
	return widget->mapTo(body, QPoint());
}

void PaintWallpaper(
		QPainter &p,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> widget,
		QRect clip) {
	const auto fill = WallpaperSize(controller);
	const auto origin = WallpaperOrigin(controller, widget);
	if (fill.isEmpty() || !origin) {
		p.fillRect(clip, st::windowBg);
		return;
	}
	const auto theme = controller->currentChatTheme();
	p.save();
	p.translate(-*origin);
	Window::SectionWidget::PaintBackground(
		p,
		theme,
		fill,
		clip.translated(*origin),
		controller->isGifPausedAtLeastFor(Window::GifPauseReason::Any));
	p.restore();
	TrackBackdrop(theme, fill);
}

void PaintGlass(
		QPainter &p,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> widget,
		QRect rect,
		int radius,
		int slot) {
	if (rect.isEmpty()) {
		return;
	}
	const auto fill = WallpaperSize(controller);
	const auto origin = WallpaperOrigin(controller, widget);
	if (fill.isEmpty() || !origin) {
		p.fillRect(rect, st::windowBg);
		return;
	}
	const auto &blurred = Blurred(controller->currentChatTheme(), fill);
	auto &cache = PanelCacheFor(widget, slot);
	const auto position = *origin + rect.topLeft();
	if (cache.serial != blurred.serial
		|| cache.size != rect.size()
		|| cache.position != position
		|| cache.radius != radius
		|| cache.ratio != style::DevicePixelRatio()
		|| cache.tint != st::windowBg->c.rgba()
		|| cache.settings != SettingsKey()) {
		RenderPanel(cache, blurred, rect.size(), position, radius);
	}
	p.drawImage(rect.topLeft(), cache.image);
}

void TrackBackdrop(not_null<Ui::ChatTheme*> theme, QSize fill) {
	// Keys are kept per theme and size, so windows of different sizes
	// do not make each other repaint
	using Key = std::tuple<const Ui::ChatTheme*, int, int>;
	static auto tracked = std::map<Key, qint64>();

	const auto key = BackgroundKey(theme, theme->backgroundState(fill));
	auto &known = tracked[Key{ theme.get(), fill.width(), fill.height() }];
	if (known == key) {
		return;
	}
	known = key;
	if (int(tracked.size()) > kMaxTrackedBackdrops) {
		tracked.clear();
	}

	// A new wallpaper fades in, the gaps are painted again once it is shown
	crl::on_main([] {
		BackdropChangesStream().fire({});
	});
	QTimer::singleShot(kBackdropFadeDuration, [] {
		BackdropChangesStream().fire({});
	});
}

rpl::producer<> BackdropChanges() {
	return rpl::merge(BackdropChangesStream().events(), SettingsChanges());
}

void SetFloatingPanel(
		not_null<Window::SessionController*> controller,
		not_null<Ui::RpWidget*> panel,
		bool floating) {
	const auto was = panel->property(kFloatingProperty);
	panel->setProperty(kFloatingProperty, floating);
	if (was.isValid()) {
		const auto corners = panel->findChildren<QWidget*>(
			kCornerName,
			Qt::FindDirectChildrenOnly);
		for (const auto corner : corners) {
			corner->setVisible(floating);
			if (floating) {
				corner->raise();
			}
		}
		if (was.toBool() != floating) {
			panel->update();
		}
		return;
	}

	const auto positions = {
		std::pair{ true, true },
		std::pair{ false, true },
		std::pair{ true, false },
		std::pair{ false, false },
	};
	for (const auto &position : positions) {
		const auto left = position.first;
		const auto top = position.second;
		const auto corner = Ui::CreateChild<Ui::RpWidget>(panel.get());
		corner->setObjectName(kCornerName);
		corner->setAttribute(Qt::WA_TransparentForMouseEvents);
		corner->setVisible(floating);
		rpl::combine(
			panel->sizeValue(),
			rpl::single(rpl::empty) | rpl::then(SettingsChanges())
		) | rpl::on_next([=](QSize size, auto) {
			const auto radius = GlassRadius();
			corner->setGeometry(
				left ? 0 : (size.width() - radius),
				top ? 0 : (size.height() - radius),
				radius,
				radius);
			corner->raise();
		}, corner->lifetime());
		corner->paintRequest(
		) | rpl::on_next([=] {
			const auto radius = corner->width();
			auto p = QPainter(corner);
			PaintCorner(
				p,
				controller,
				corner,
				QPoint(left ? radius : 0, top ? radius : 0));
		}, corner->lifetime());
	}
	BackdropChanges(
	) | rpl::on_next([=] {
		panel->update();
	}, panel->lifetime());

	// New children are added on top, the corners stay above them
	const auto raiseCorners = [=] {
		const auto corners = panel->findChildren<QWidget*>(
			kCornerName,
			Qt::FindDirectChildrenOnly);
		for (const auto corner : corners) {
			corner->raise();
		}
	};
	base::install_event_filter(panel, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::ChildAdded) {
			Ui::PostponeCall(panel.get(), raiseCorners);
		}
		return base::EventFilterResult::Continue;
	}, panel->lifetime());
}

int PanelRadius(not_null<const QWidget*> panel) {
	return panel->property(kFloatingProperty).toBool() ? GlassRadius() : 0;
}

void PaintRowBackground(
		QPainter &p,
		QRect rect,
		bool active,
		bool selected) {
	if (!active && !selected) {
		return;
	}
	auto color = st::dialogsBgActive->c;
	if (!active) {
		color = st::windowFg->c;
		color.setAlphaF(kRowOverAlpha);
	}
	const auto inset = style::ConvertScale(kRowInset);
	const auto radius = style::ConvertScale(kRowRadius);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRoundedRect(
		rect.marginsRemoved({ inset, 0, inset, 0 }),
		radius,
		radius);
}

int GlassMargin() {
	return GlassGaps() ? style::ConvertScale(kMargin) : 0;
}

int GlassRadius() {
	return style::ConvertScale(GlassRadiusSetting());
}

int BarRadius() {
	return GlassGaps() ? GlassRadius() : 0;
}

int ComposeSkip() {
	return GlassMargin() + st::historySendRight;
}

bool GlassEnabled() {
	return OptionGlass.value();
}

int GlassTint() {
	return std::clamp(OptionGlassTint.value(), kMinTint, kMaxTint);
}

int GlassBlur() {
	return std::clamp(OptionGlassBlur.value(), 0, kMaxBlur);
}

int GlassRadiusSetting() {
	return std::clamp(OptionGlassRadius.value(), 0, kMaxRadius);
}

bool GlassGaps() {
	return OptionGlassGaps.value();
}

void SetGlassEnabled(bool enabled) {
	OptionGlass.set(enabled);
}

void SetGlassTint(int tint) {
	OptionGlassTint.set(tint);
}

void SetGlassBlur(int blur) {
	OptionGlassBlur.set(blur);
}

void SetGlassRadius(int radius) {
	OptionGlassRadius.set(radius);
}

void SetGlassGaps(bool gaps) {
	OptionGlassGaps.set(gaps);
}

rpl::producer<> SettingsChanges() {
	return rpl::merge(
		OptionGlass.changes(),
		OptionGlassTint.changes(),
		OptionGlassBlur.changes(),
		OptionGlassRadius.changes(),
		OptionGlassGaps.changes());
}

void ApplyDefaultsOnce() {
	if (!OptionLayoutApplied.value()) {
		OptionLayoutApplied.set(true);
		Core::App().settings().setChatFiltersHorizontal(true);
		Core::App().saveSettingsDelayed();
	}

	using namespace Window::Theme;
	if (OptionDayThemeApplied.value() || IsNightMode()) {
		return;
	}
	OptionDayThemeApplied.set(true);
	if (Background()->themeObject().pathAbsolute.isEmpty()) {
		ApplyDefaultWithPath(kDayThemePath);
		KeepApplied();
	}
}

void PaintBar(
		QPainter &p,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> widget,
		QRect rect) {
	const auto margin = GlassMargin();
	PaintWallpaper(p, controller, widget, rect);
	PaintGlass(
		p,
		controller,
		widget,
		rect.marginsRemoved({ margin, margin / 2, margin, margin / 2 }),
		BarRadius());
}

} // namespace Gls
