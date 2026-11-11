/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gls/gls_proxy_section.h"

#include "boxes/connection_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "core/gls_ws_proxy.h"
#include "gls/gls_lang.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Gls {
namespace {

using Core::GlsProxyCheckResult;
using Core::GlsProxyConnection;
using Core::GlsProxyRoute;
using Core::GlsProxySettings;
using Core::GlsProxyState;
using Core::GlsWsProxy;

constexpr auto kSecondsInMinute = 60;
constexpr auto kSecondsInHour = 3600;
constexpr auto kTilesCount = 3;
constexpr auto kLogLinesShown = 200;
constexpr auto kShieldSize = 24.;
constexpr auto kBgAlpha = 0.18;
constexpr auto kBorderAlpha = 0.35;
constexpr auto kIconAlpha = 0.22;
constexpr auto kTileAlpha = 0.07;
constexpr auto kChipAlpha = 0.18;
constexpr auto kPoolSizes = std::array{ 0, 1, 2, 4, 6, 8, 12, 16 };

const auto kOkColor = QColor(63, 191, 127);
const auto kSlowColor = QColor(230, 170, 40);
const auto kBadColor = QColor(235, 87, 87);

[[nodiscard]] QColor WithAlpha(QColor color, float64 alpha) {
	color.setAlphaF(alpha);
	return color;
}

[[nodiscard]] QString RouteText(const QString &route) {
	return (route == u"ws"_q)
		? u"WebSocket"_q
		: (route == u"cf"_q)
		? u"Cloudflare"_q
		: Tr("Недоступен", "Unavailable");
}

[[nodiscard]] QColor RouteColor(const QString &route) {
	return (route == u"ws"_q)
		? kOkColor
		: (route == u"cf"_q)
		? kSlowColor
		: kBadColor;
}

[[nodiscard]] QString Milliseconds(int value) {
	return Tr("%1 мс", "%1 ms").arg(value);
}

[[nodiscard]] QString Uptime(crl::time startedAt) {
	if (!startedAt) {
		return u"—"_q;
	}
	const auto seconds = int((crl::now() - startedAt) / 1000);
	if (seconds < kSecondsInHour) {
		return u"%1:%2"_q
			.arg(seconds / kSecondsInMinute)
			.arg(seconds % kSecondsInMinute, 2, 10, QChar('0'));
	}
	return Tr("%1 ч %2 мин", "%1 h %2 min")
		.arg(seconds / kSecondsInHour)
		.arg((seconds % kSecondsInHour) / kSecondsInMinute);
}

[[nodiscard]] QString DcAddress(int dc) {
	const auto proxy = GlsWsProxy::Instance();
	const auto custom = proxy ? proxy->settings().dcAddresses : QString();
	const auto lines = (custom.trimmed().isEmpty()
		? GlsWsProxy::DefaultDcAddresses()
		: custom).split(QRegularExpression(u"[\\s,;]+"_q), Qt::SkipEmptyParts);
	for (const auto &line : lines) {
		if (line.section(QChar(':'), 0, 0).toInt() == dc) {
			return line.section(QChar(':'), 1);
		}
	}
	return u"—"_q;
}

void PaintShield(QPainter &p, QRectF rect, const QColor &color) {
	const auto scale = rect.width() / kShieldSize;
	auto path = QPainterPath();
	path.moveTo(12, 3);
	path.lineTo(5, 6);
	path.lineTo(5, 11.5);
	path.cubicTo(5, 15.8, 8, 19.5, 12, 21);
	path.cubicTo(16, 19.5, 19, 15.8, 19, 11.5);
	path.lineTo(19, 6);
	path.closeSubpath();
	auto check = QPainterPath();
	check.moveTo(9, 12);
	check.lineTo(11.2, 14.2);
	check.lineTo(15.5, 10);

	p.save();
	p.translate(rect.topLeft());
	p.scale(scale, scale);
	auto pen = QPen(color, 1.9);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
	p.drawPath(check);
	p.restore();
}

class StatusCard final : public Ui::RpWidget {
public:
	StatusCard(
		QWidget *parent,
		not_null<GlsWsProxy*> proxy,
		Fn<void()> check);

private:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

	[[nodiscard]] QColor color() const;
	[[nodiscard]] QString title() const;
	[[nodiscard]] QString subtitle() const;

	const not_null<GlsWsProxy*> _proxy;
	const not_null<Ui::RoundButton*> _check;
	const not_null<Ui::RoundButton*> _restart;
	base::Timer _timer;

};

StatusCard::StatusCard(
	QWidget *parent,
	not_null<GlsWsProxy*> proxy,
	Fn<void()> check)
: RpWidget(parent)
, _proxy(proxy)
, _check(Ui::CreateChild<Ui::RoundButton>(
	this,
	rpl::single(Tr("Проверить соединение", "Check connection")),
	st::glsProxyCardButton))
, _restart(Ui::CreateChild<Ui::RoundButton>(
	this,
	rpl::single(Tr("Перезапустить", "Restart")),
	st::glsProxyCardButton))
, _timer([=] { update(); }) {
	_check->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_restart->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_check->setClickedCallback(check);
	_restart->setClickedCallback([=] { _proxy->restart(); });

	rpl::merge(
		_proxy->stateValue() | rpl::to_empty,
		_proxy->connectionValue() | rpl::to_empty,
		_proxy->checkUpdates()
	) | rpl::on_next([=] {
		const auto running = (_proxy->state() == GlsProxyState::Running);
		if (running && !_timer.isActive()) {
			_timer.callEach(crl::time(1000));
		} else if (!running) {
			_timer.cancel();
		}
		_restart->setVisible(
			_proxy->connection() == GlsProxyConnection::BuiltIn);
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}, lifetime());
}

QColor StatusCard::color() const {
	if (_proxy->connection() != GlsProxyConnection::BuiltIn) {
		return st::windowSubTextFg->c;
	}
	switch (_proxy->state()) {
	case GlsProxyState::Running:
		return _proxy->fastestRoute() ? kOkColor : (_proxy->check().done
			? kBadColor
			: kOkColor);
	case GlsProxyState::Starting:
		return kSlowColor;
	case GlsProxyState::Failed:
	case GlsProxyState::Missing:
		return kBadColor;
	case GlsProxyState::Stopped:
		return st::windowSubTextFg->c;
	}
	Unexpected("State in StatusCard::color.");
}

QString StatusCard::title() const {
	if (_proxy->state() == GlsProxyState::Missing) {
		return Tr("Модуль прокси не найден", "The proxy module is missing");
	} else if (_proxy->connection() != GlsProxyConnection::BuiltIn) {
		return Tr("Встроенный прокси выключен", "The built-in proxy is off");
	}
	switch (_proxy->state()) {
	case GlsProxyState::Running:
		return Tr("Встроенный прокси работает", "The built-in proxy works");
	case GlsProxyState::Starting:
		return Tr("Прокси запускается…", "The proxy is starting…");
	case GlsProxyState::Failed:
		return Tr("Прокси не запустился", "The proxy failed to start");
	case GlsProxyState::Stopped:
	case GlsProxyState::Missing:
		break;
	}
	return Tr("Прокси остановлен", "The proxy is stopped");
}

QString StatusCard::subtitle() const {
	if (_proxy->state() == GlsProxyState::Missing) {
		return Tr(
			"Переустановите Telegram GLS",
			"Reinstall Telegram GLS");
	} else if (_proxy->connection() != GlsProxyConnection::BuiltIn) {
		return Tr(
			"Выберите его в разделе «Тип соединения»",
			"Choose it under Connection type");
	} else if (_proxy->state() == GlsProxyState::Failed) {
		return _proxy->error();
	} else if (_proxy->check().running) {
		return Tr("Проверяю соединение…", "Checking the connection…");
	} else if (const auto route = _proxy->fastestRoute()) {
		return Tr("Через %1 · пинг %2", "Via %1 · ping %2")
			.arg(RouteText(route->route))
			.arg(Milliseconds(route->milliseconds));
	} else if (_proxy->check().done) {
		return Tr(
			"Telegram недоступен ни по одному маршруту",
			"Telegram is unreachable by every route");
	}
	return u"127.0.0.1:%1"_q.arg(_proxy->settings().port);
}

int StatusCard::resizeGetHeight(int newWidth) {
	const auto &padding = st::glsProxyCardPadding;
	const auto inner = newWidth - padding.left() - padding.right();
	const auto skip = st::glsProxyCardTileSkip;
	const auto top = padding.top()
		+ st::glsProxyCardIcon
		+ st::glsProxyCardTileTop
		+ st::glsProxyCardTileHeight
		+ st::glsProxyCardButtonsTop;
	if (_restart->isHidden()) {
		_check->setFullWidth(inner);
	} else {
		_check->setFullWidth((inner - skip) / 2);
		_restart->setFullWidth(inner - skip - _check->width());
		_restart->moveToLeft(
			padding.left() + _check->width() + skip,
			top,
			newWidth);
	}
	_check->moveToLeft(padding.left(), top, newWidth);
	return top + _check->height() + padding.bottom();
}

void StatusCard::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	PainterHighQualityEnabler hq(p);

	const auto &padding = st::glsProxyCardPadding;
	const auto radius = st::glsProxyCardRadius;
	const auto color = this->color();
	const auto full = QRectF(rect()).marginsRemoved({ 0.5, 0.5, 0.5, 0.5 });
	p.setPen(QPen(WithAlpha(color, kBorderAlpha), 1.));
	p.setBrush(WithAlpha(color, kBgAlpha));
	p.drawRoundedRect(full, radius, radius);

	const auto size = st::glsProxyCardIcon;
	const auto icon = QRect(padding.left(), padding.top(), size, size);
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(color, kIconAlpha));
	p.drawEllipse(icon);
	const auto shield = size * 0.56;
	PaintShield(
		p,
		QRectF(
			icon.x() + (size - shield) / 2.,
			icon.y() + (size - shield) / 2.,
			shield,
			shield),
		color);

	const auto textLeft = icon.x() + size + st::glsProxyCardIconSkip;
	const auto textWidth = width() - textLeft - padding.right();
	const auto &titleFont = st::glsProxyCardTitle;
	const auto &textFont = st::glsProxyCardText;
	const auto textTop = icon.y()
		+ (size - titleFont->height - textFont->height) / 2;
	p.setPen(st::windowFg);
	p.setFont(titleFont);
	p.drawText(
		textLeft,
		textTop + titleFont->ascent,
		titleFont->elided(title(), textWidth));
	p.setPen(st::windowSubTextFg);
	p.setFont(textFont);
	p.drawText(
		textLeft,
		textTop + titleFont->height + textFont->ascent,
		textFont->elided(subtitle(), textWidth));

	const auto route = _proxy->fastestRoute();
	const auto running = (_proxy->state() == GlsProxyState::Running);
	const auto tiles = std::array<std::pair<QString, QString>, kTilesCount>{ {
		{
			Tr("Маршрут", "Route"),
			(running && route) ? RouteText(route->route) : u"—"_q,
		},
		{
			Tr("Пинг", "Ping"),
			(running && route) ? Milliseconds(route->milliseconds) : u"—"_q,
		},
		{
			Tr("Работает", "Uptime"),
			Uptime(_proxy->startedAt()),
		},
	} };
	const auto skip = st::glsProxyCardTileSkip;
	const auto inner = width() - padding.left() - padding.right();
	const auto tileWidth = (inner - skip * (kTilesCount - 1)) / kTilesCount;
	const auto tileTop = icon.y() + size + st::glsProxyCardTileTop;
	const auto tileHeight = st::glsProxyCardTileHeight;
	const auto tileRadius = st::glsProxyCardTileRadius;
	const auto tilePadding = st::glsProxyCardTileSkip + skip / 2;
	for (auto i = 0; i != kTilesCount; ++i) {
		const auto tile = QRect(
			padding.left() + i * (tileWidth + skip),
			tileTop,
			tileWidth,
			tileHeight);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(st::windowFg->c, kTileAlpha));
		p.drawRoundedRect(tile, tileRadius, tileRadius);
		const auto left = tile.x() + tilePadding;
		const auto available = tile.width() - 2 * tilePadding;
		p.setFont(textFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			tile.y() + tilePadding + textFont->ascent,
			textFont->elided(tiles[i].first, available));
		p.setFont(titleFont);
		p.setPen(st::windowFg);
		p.drawText(
			left,
			tile.y() + tile.height() - tilePadding - titleFont->descent,
			titleFont->elided(tiles[i].second, available));
	}
}

class DcTable final : public Ui::RpWidget {
public:
	DcTable(QWidget *parent, not_null<GlsWsProxy*> proxy);

private:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

	const not_null<GlsWsProxy*> _proxy;

};

DcTable::DcTable(QWidget *parent, not_null<GlsWsProxy*> proxy)
: RpWidget(parent)
, _proxy(proxy) {
	_proxy->checkUpdates(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

int DcTable::resizeGetHeight(int) {
	return GlsWsProxy::kDcCount * st::glsProxyDcHeight;
}

void DcTable::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	PainterHighQualityEnabler hq(p);

	const auto &padding = st::glsProxyDcPadding;
	const auto height = st::glsProxyDcHeight;
	const auto badge = st::glsProxyDcBadge;
	const auto radius = st::glsProxyDcBadgeRadius;
	const auto &titleFont = st::glsProxyCardTitle;
	const auto &textFont = st::glsProxyCardText;
	const auto chipHeight = st::glsProxyDcChipHeight;
	const auto chipPadding = st::glsProxyDcChipPadding;
	const auto &check = _proxy->check();
	for (auto dc = 1; dc <= GlsWsProxy::kDcCount; ++dc) {
		const auto top = (dc - 1) * height;
		const auto badgeRect = QRect(
			padding.left(),
			top + (height - badge) / 2,
			badge,
			badge);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(st::windowFg->c, kTileAlpha));
		p.drawRoundedRect(badgeRect, radius, radius);
		p.setPen(st::windowFg);
		p.setFont(st::semiboldFont);
		p.drawText(badgeRect, Qt::AlignCenter, u"DC%1"_q.arg(dc));

		const auto i = ranges::find(check.results, dc, &GlsProxyCheckResult::dc);
		const auto result = (i != end(check.results)) ? &*i : nullptr;
		auto chip = QString();
		auto route = check.running
			? Tr("Проверяется…", "Checking…")
			: Tr("Не проверялся", "Not checked");
		if (result) {
			route = RouteText(result->route);
			if (result->route != u"fail"_q) {
				chip = Milliseconds(result->milliseconds);
			} else if (!result->details.isEmpty()) {
				route += u" · "_q + result->details;
			}
		}
		const auto chipWidth = chip.isEmpty()
			? 0
			: (textFont->width(chip) + 2 * chipPadding);
		const auto left = badgeRect.x() + badge + padding.left() / 2;
		const auto available = width()
			- left
			- padding.right()
			- (chipWidth ? (chipWidth + chipPadding) : 0);
		p.setFont(st::normalFont);
		p.setPen(st::windowFg);
		p.drawText(
			left,
			top + padding.top() + st::normalFont->ascent,
			st::normalFont->elided(DcAddress(dc), available));
		p.setFont(textFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			top + height - padding.bottom() - textFont->descent,
			textFont->elided(route, available));
		if (chipWidth) {
			const auto color = RouteColor(result->route);
			const auto rect = QRect(
				width() - padding.right() - chipWidth,
				top + (height - chipHeight) / 2,
				chipWidth,
				chipHeight);
			p.setPen(Qt::NoPen);
			p.setBrush(WithAlpha(color, kChipAlpha));
			p.drawRoundedRect(rect, chipHeight / 2., chipHeight / 2.);
			p.setPen(color);
			p.drawText(rect, Qt::AlignCenter, chip);
		}
	}
}

void AddRadioAbout(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::glsProxyRadioAbout),
		st::glsProxyRadioAboutPadding);
}

template <typename Enum>
void AddRadio(
		not_null<Ui::VerticalLayout*> container,
		const std::shared_ptr<Ui::RadioenumGroup<Enum>> &group,
		Enum value,
		const QString &text,
		const QString &about) {
	container->add(
		object_ptr<Ui::Radioenum<Enum>>(
			container,
			group,
			value,
			text,
			st::glsProxyRadio),
		st::glsProxyRadioPadding);
	if (!about.isEmpty()) {
		AddRadioAbout(container, about);
	}
}

not_null<Ui::SettingsButton*> AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		bool current,
		Fn<void(bool)> changed) {
	const auto button = Settings::AddButtonWithIcon(
		container,
		rpl::single(text),
		st::settingsButtonNoIcon
	)->toggleOn(rpl::single(current));
	button->toggledChanges(
	) | rpl::on_next(changed, button->lifetime());
	return button;
}

void AddField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value,
		Fn<void(QString)> save) {
	const auto field = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::glsProxyField,
			rpl::single(placeholder),
			value),
		st::glsProxyFieldPadding);
	const auto apply = [=] {
		save(field->getLastText().trimmed());
	};
	field->submits() | rpl::on_next(apply, field->lifetime());
	field->focusedChanges(
	) | rpl::filter(!rpl::mappers::_1) | rpl::on_next(apply, field->lifetime());
}

void UpdateSettings(Fn<void(GlsProxySettings&)> change) {
	if (const auto proxy = GlsWsProxy::Instance()) {
		auto settings = proxy->settings();
		change(settings);
		proxy->updateSettings(std::move(settings));
	}
}

void ShowTextBox(
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QString &placeholder,
		const QString &value,
		bool multiline,
		Fn<void(QString)> save) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			multiline
				? Ui::InputField::Mode::MultiLine
				: Ui::InputField::Mode::SingleLine,
			rpl::single(placeholder),
			value));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto submit = [=] {
			save(field->getLastText().trimmed());
			box->closeBox();
		};
		if (!multiline) {
			field->submits() | rpl::on_next(submit, field->lifetime());
		}
		box->addButton(rpl::single(Tr("Сохранить", "Save")), submit);
		box->addButton(rpl::single(Tr("Отмена", "Cancel")), [=] {
			box->closeBox();
		});
	}));
}

void ShowLogBox(not_null<Window::SessionController*> controller) {
	const auto proxy = GlsWsProxy::Instance();
	if (!proxy) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(Tr("Журнал прокси", "Proxy log")));
		box->setWidth(st::boxWideWidth);
		const auto text = [=] {
			const auto &log = proxy->log();
			const auto from = (log.size() > kLogLinesShown)
				? (log.size() - kLogLinesShown)
				: 0;
			auto lines = QStringList();
			for (auto i = from; i != log.size(); ++i) {
				lines.push_back(log[i]);
			}
			return lines.isEmpty()
				? Tr("Журнал пока пуст", "The log is empty")
				: lines.join(QChar('\n'));
		};
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			proxy->logUpdates() | rpl::map(text) | rpl::start_with(text()),
			st::glsProxyLogLabel));
		label->setSelectable(true);
		box->addButton(rpl::single(Tr("Скопировать", "Copy")), [=] {
			QGuiApplication::clipboard()->setText(text());
		});
		box->addButton(rpl::single(Tr("Закрыть", "Close")), [=] {
			box->closeBox();
		});
	}));
}

void OpenCustomProxies(not_null<Window::SessionController*> controller) {
	controller->show(ProxiesBoxController::CreateOwningBox(
		&controller->session().account()));
}

[[nodiscard]] bool HasCustomProxies(not_null<GlsWsProxy*> proxy) {
	const auto ours = proxy->proxyData();
	for (const auto &item : Core::App().settings().proxy().list()) {
		if (item != ours) {
			return true;
		}
	}
	return false;
}

void SetupConnection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<GlsWsProxy*> proxy) {
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Тип соединения", "Connection type")));
	const auto group = std::make_shared<
		Ui::RadioenumGroup<GlsProxyConnection>>(proxy->connection());
	AddRadio(
		container,
		group,
		GlsProxyConnection::BuiltIn,
		Tr("Встроенный tg-ws-proxy", "Built-in tg-ws-proxy"),
		Tr(
			"Рекомендуется, если Telegram работает с перебоями",
			"Recommended when Telegram connects poorly"));
	AddRadio(
		container,
		group,
		GlsProxyConnection::Custom,
		Tr("Свой прокси", "Custom proxy"),
		Tr(
			"SOCKS5, HTTP или MTProto из списка ниже",
			"SOCKS5, HTTP or MTProto from the list below"));
	AddRadio(
		container,
		group,
		GlsProxyConnection::System,
		Tr("Системный прокси Windows", "System proxy"),
		QString());
	AddRadio(
		container,
		group,
		GlsProxyConnection::Direct,
		Tr("Без прокси", "No proxy"),
		QString());
	group->setChangedCallback([=](GlsProxyConnection value) {
		if (value == proxy->connection()) {
			return;
		} else if (value == GlsProxyConnection::Custom
			&& !HasCustomProxies(proxy)) {
			group->setValue(proxy->connection());
			OpenCustomProxies(controller);
			return;
		}
		proxy->setConnection(value);
	});
	proxy->connectionValue(
	) | rpl::on_next([=](GlsProxyConnection value) {
		group->setValue(value);
	}, container->lifetime());
	Ui::AddSkip(container);
}

void SetupRoute(
		not_null<Ui::VerticalLayout*> container,
		not_null<GlsWsProxy*> proxy) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Маршрут встроенного прокси", "Built-in proxy route")));
	const auto group = std::make_shared<Ui::RadioenumGroup<GlsProxyRoute>>(
		proxy->settings().route);
	AddRadio(
		container,
		group,
		GlsProxyRoute::Auto,
		Tr("Авто", "Auto"),
		Tr(
			"WebSocket, при сбое Cloudflare, потом прямое подключение",
			"WebSocket, then Cloudflare, then a direct connection"));
	AddRadio(
		container,
		group,
		GlsProxyRoute::CloudflareFirst,
		Tr("Сначала Cloudflare", "Cloudflare first"),
		Tr(
			"Если провайдер блокирует WebSocket к Telegram",
			"When the provider blocks WebSocket to Telegram"));
	AddRadio(
		container,
		group,
		GlsProxyRoute::WebSocketOnly,
		Tr("Только WebSocket", "WebSocket only"),
		QString());
	group->setChangedCallback([=](GlsProxyRoute value) {
		UpdateSettings([=](GlsProxySettings &settings) {
			settings.route = value;
		});
	});
	Ui::AddSkip(container);
}

void SetupCloudflare(
		not_null<Ui::VerticalLayout*> container,
		not_null<GlsWsProxy*> proxy) {
	const auto &settings = proxy->settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Cloudflare"_q));
	AddToggle(
		container,
		Tr("Резервный CF-прокси", "Cloudflare fallback"),
		settings.cloudflare,
		[=](bool value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.cloudflare = value;
			});
		});
	Settings::AddButtonWithLabel(
		container,
		rpl::single(Tr("Домены", "Domains")),
		rpl::single(rpl::empty) | rpl::then(
			proxy->checkUpdates()
		) | rpl::map([=] {
			if (!proxy->settings().cloudflareDomains.trimmed().isEmpty()) {
				return Tr("Свои", "Custom");
			}
			const auto count = proxy->check().domains;
			return count
				? Tr("Автоматически, %1", "Automatic, %1").arg(count)
				: Tr("Автоматически", "Automatic");
		}),
		st::settingsButtonNoIcon);
	Ui::AddSkip(container);
	AddField(
		container,
		Tr("Свои домены, через запятую", "Own domains, comma separated"),
		settings.cloudflareDomains,
		[=](QString value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.cloudflareDomains = value;
			});
		});
	AddField(
		container,
		Tr("Домены Cloudflare Worker", "Cloudflare Worker domains"),
		settings.workerDomains,
		[=](QString value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.workerDomains = value;
			});
		});
	AddToggle(
		container,
		Tr("HTTP/2 для медиа", "HTTP/2 for media"),
		settings.http2Media,
		[=](bool value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.http2Media = value;
			});
		});
	AddToggle(
		container,
		Tr("Без TLS (порт 80)", "Without TLS (port 80)"),
		settings.noTls,
		[=](bool value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.noTls = value;
			});
		});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(Tr(
			"Без TLS включайте, только если провайдер ломает "
			"защищённые соединения.",
			"Turn off TLS only when the provider breaks secure "
			"connections.")));
}

void SetupDataCenters(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<GlsWsProxy*> proxy) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Дата-центры Telegram", "Telegram data centers")));
	container->add(object_ptr<DcTable>(container, proxy));
	Settings::AddButtonWithIcon(
		container,
		rpl::single(Tr(
			"Изменить IP-адреса дата-центров",
			"Edit data center addresses")),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		const auto current = proxy->settings().dcAddresses;
		ShowTextBox(
			controller,
			Tr("IP-адреса дата-центров", "Data center addresses"),
			Tr("Номер:IP, по одному на строку", "Number:IP, one per line"),
			current.isEmpty() ? GlsWsProxy::DefaultDcAddresses() : current,
			true,
			[=](QString value) {
				UpdateSettings([=](GlsProxySettings &settings) {
					settings.dcAddresses = (value
						== GlsWsProxy::DefaultDcAddresses())
						? QString()
						: value;
				});
			});
	});
	Ui::AddSkip(container);
}

void SetupBypass(
		not_null<Ui::VerticalLayout*> container,
		not_null<GlsWsProxy*> proxy) {
	const auto &settings = proxy->settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Обход блокировок", "Bypassing blocks")));
	AddField(
		container,
		Tr("Домен для подмены SNI", "Fronting domain for SNI"),
		settings.frontingDomain,
		[=](QString value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.frontingDomain = value;
			});
		});
	AddField(
		container,
		Tr(
			"Внешний прокси, socks5://адрес:порт",
			"Outbound proxy, socks5://host:port"),
		settings.outboundProxy,
		[=](QString value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.outboundProxy = value;
			});
		});
	AddField(
		container,
		Tr(
			"Резервный MTProto-прокси, хост:порт:ключ",
			"Fallback MTProto proxy, host:port:secret"),
		settings.mtprotoFallback,
		[=](QString value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.mtprotoFallback = value;
			});
		});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(Tr(
			"Эти поля нужны, только если «Авто» не помогает. Пустые "
			"поля не используются.",
			"These fields matter only when Auto does not help. Empty "
			"fields are not used.")));
}

void SetupAdvanced(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<GlsWsProxy*> proxy) {
	const auto &settings = proxy->settings();
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Дополнительно", "Advanced")));
	const auto changes = container->lifetime().make_state<
		rpl::event_stream<>>();
	const auto value = [=](Fn<QString(const GlsProxySettings&)> text) {
		return rpl::single(rpl::empty) | rpl::then(
			changes->events()
		) | rpl::map([=] {
			return text(proxy->settings());
		});
	};
	Settings::AddButtonWithLabel(
		container,
		rpl::single(Tr("Порт", "Port")),
		value([](const GlsProxySettings &settings) {
			return QString::number(settings.port);
		}),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		ShowTextBox(
			controller,
			Tr("Порт прокси", "Proxy port"),
			Tr("Порт", "Port"),
			QString::number(proxy->settings().port),
			false,
			[=](QString text) {
				const auto port = text.toInt();
				if (port > 0 && port <= 65535) {
					UpdateSettings([=](GlsProxySettings &settings) {
						settings.port = port;
					});
					changes->fire({});
				}
			});
	});
	Settings::AddButtonWithLabel(
		container,
		rpl::single(Tr("Ключ", "Secret")),
		rpl::single(u"••••••••"_q),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = rpl::single(Tr(
				"Сменить ключ встроенного прокси? Telegram GLS сразу "
				"переподключится с новым ключом.",
				"Change the key of the built-in proxy? Telegram GLS "
				"reconnects with the new key at once.")),
			.confirmed = [=](Fn<void()> close) {
				proxy->regenerateSecret();
				close();
			},
			.confirmText = rpl::single(Tr("Сменить", "Change")),
		}));
	});
	Settings::AddButtonWithLabel(
		container,
		rpl::single(Tr(
			"Готовых WebSocket-соединений",
			"Pre-warmed WebSocket connections")),
		value([](const GlsProxySettings &settings) {
			return QString::number(settings.poolSize);
		}),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		auto options = std::vector<QString>();
		for (const auto size : kPoolSizes) {
			options.push_back(QString::number(size));
		}
		const auto current = ranges::find(
			kPoolSizes,
			proxy->settings().poolSize);
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			SingleChoiceBox(box, {
				.title = rpl::single(Tr(
					"Готовых соединений на дата-центр",
					"Connections per data center")),
				.options = options,
				.initialSelection = (current != end(kPoolSizes))
					? int(current - begin(kPoolSizes))
					: 0,
				.callback = [=](int index) {
					UpdateSettings([=](GlsProxySettings &settings) {
						settings.poolSize = kPoolSizes[index];
					});
					changes->fire({});
				},
			});
		}));
	});
	AddToggle(
		container,
		Tr("Подробный журнал", "Verbose log"),
		settings.verbose,
		[=](bool value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.verbose = value;
			});
		});
	AddToggle(
		container,
		Tr("Раздавать прокси в локальную сеть", "Share in the local network"),
		settings.shareInNetwork,
		[=](bool value) {
			UpdateSettings([=](GlsProxySettings &settings) {
				settings.shareInNetwork = value;
			});
		});
	Settings::AddButtonWithIcon(
		container,
		rpl::single(Tr("Открыть журнал", "Open the log")),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		ShowLogBox(controller);
	});
	Ui::AddSkip(container);
}

void SetupCustom(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(Tr("Свои прокси", "Custom proxies")));
	Settings::AddButtonWithIcon(
		container,
		rpl::single(Tr("Список своих прокси", "Custom proxy list")),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		OpenCustomProxies(controller);
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(Tr(
			"Встроенный прокси на основе tg-ws-proxy-rs работает внутри "
			"Telegram GLS, отдельной программы и окна у него нет. Звонки "
			"идут мимо прокси.",
			"The built-in proxy based on tg-ws-proxy-rs runs inside "
			"Telegram GLS, with no separate program or window. Calls "
			"bypass the proxy.")));
}

class ProxySection final : public Settings::Section<ProxySection> {
public:
	ProxySection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

ProxySection::ProxySection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> ProxySection::title() {
	return rpl::single(Tr("Прокси", "Proxy"));
}

void ProxySection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto controller = this->controller();
	const auto proxy = GlsWsProxy::Instance();
	if (!proxy) {
		Ui::AddSkip(content);
		Ui::AddDividerText(
			content,
			rpl::single(Tr(
				"Встроенный прокси недоступен.",
				"The built-in proxy is unavailable.")));
		Ui::ResizeFitChild(this, content);
		return;
	}
	content->add(
		object_ptr<StatusCard>(
			content,
			proxy,
			[=] { ShowProxyCheckBox(controller); }),
		st::glsProxyCardMargin);
	Ui::AddSkip(content);
	SetupConnection(content, controller, proxy);
	SetupRoute(content, proxy);
	SetupCloudflare(content, proxy);
	SetupDataCenters(content, controller, proxy);
	SetupBypass(content, proxy);
	SetupAdvanced(content, controller, proxy);
	SetupCustom(content, controller);

	if (!proxy->check().done && !proxy->check().running) {
		proxy->runCheck();
	}
	Ui::ResizeFitChild(this, content);
}

} // namespace

Settings::Type ProxySectionId() {
	return ProxySection::Id();
}

void ShowProxyCheckBox(not_null<Window::SessionController*> controller) {
	const auto proxy = GlsWsProxy::Instance();
	if (!proxy) {
		return;
	}
	proxy->runCheck();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(
			Tr("Проверка соединения", "Connection check")));
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(Tr(
					"Каждый дата-центр Telegram проверяется по всем "
					"настроенным маршрутам",
					"Every Telegram data center is checked by each "
					"configured route")),
				st::glsProxyRadioAbout));
		box->addRow(
			object_ptr<DcTable>(box, proxy),
			style::margins());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(rpl::empty) | rpl::then(
				proxy->checkUpdates()
			) | rpl::map([=] {
				const auto &check = proxy->check();
				if (check.running || !check.done) {
					return Tr("Проверяю…", "Checking…");
				}
				const auto ok = ranges::count_if(
					check.results,
					[](const GlsProxyCheckResult &result) {
						return result.route != u"fail"_q;
					});
				const auto viaCloudflare = ranges::count(
					check.results,
					u"cf"_q,
					&GlsProxyCheckResult::route);
				if (ok == GlsWsProxy::kDcCount) {
					return viaCloudflare
						? Tr(
							"Всё работает. Часть дата-центров идёт через "
							"Cloudflare, это нормально.",
							"Everything works. Some data centers go "
							"through Cloudflare, this is fine.")
						: Tr("Всё работает.", "Everything works.");
				} else if (ok) {
					return Tr(
						"Часть дата-центров недоступна. Попробуйте "
						"маршрут «Сначала Cloudflare».",
						"Some data centers are unreachable. Try the "
						"Cloudflare first route.");
				}
				return Tr(
					"Telegram недоступен. Проверьте интернет или укажите "
					"свои домены Cloudflare.",
					"Telegram is unreachable. Check the internet or set "
					"your own Cloudflare domains.");
			}),
			st::boxLabel));
		box->addButton(rpl::single(Tr("Готово", "Done")), [=] {
			box->closeBox();
		});
		box->addLeftButton(
			rpl::single(Tr("Скопировать отчёт", "Copy report")),
			[=] {
				auto lines = QStringList();
				for (const auto &result : proxy->check().results) {
					lines.push_back(u"DC%1\t%2\t%3\t%4"_q
						.arg(result.dc)
						.arg(result.route)
						.arg(result.milliseconds)
						.arg(result.details));
				}
				QGuiApplication::clipboard()->setText(
					lines.join(QChar('\n')));
			});
	}));
}

rpl::producer<QString> ProxyStatusValue() {
	const auto proxy = GlsWsProxy::Instance();
	if (!proxy) {
		return rpl::single(QString());
	}
	return rpl::combine(
		proxy->stateValue(),
		proxy->connectionValue()
	) | rpl::map([=](GlsProxyState state, GlsProxyConnection connection) {
		switch (connection) {
		case GlsProxyConnection::Custom: return Tr("Свой", "Custom");
		case GlsProxyConnection::System: return Tr("Системный", "System");
		case GlsProxyConnection::Direct: return Tr("Выключен", "Off");
		case GlsProxyConnection::BuiltIn: break;
		}
		switch (state) {
		case GlsProxyState::Running: return Tr("Подключено", "Connected");
		case GlsProxyState::Starting: return Tr("Запуск…", "Starting…");
		case GlsProxyState::Failed:
		case GlsProxyState::Missing: return Tr("Ошибка", "Error");
		case GlsProxyState::Stopped: break;
		}
		return Tr("Остановлен", "Stopped");
	});
}

rpl::producer<bool> ProxyHealthyValue() {
	const auto proxy = GlsWsProxy::Instance();
	if (!proxy) {
		return rpl::single(false);
	}
	return rpl::combine(
		proxy->stateValue(),
		proxy->connectionValue()
	) | rpl::map([](GlsProxyState state, GlsProxyConnection connection) {
		return (connection != GlsProxyConnection::BuiltIn)
			|| (state == GlsProxyState::Running);
	});
}

} // namespace Gls
