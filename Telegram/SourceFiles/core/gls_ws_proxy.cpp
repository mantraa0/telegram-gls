/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/gls_ws_proxy.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "base/random.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibrary>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

namespace Core {
namespace {

constexpr auto kEventLog = 1;
constexpr auto kEventListening = 2;
constexpr auto kEventStopped = 3;
constexpr auto kEventError = 4;
constexpr auto kEventCheck = 5;
constexpr auto kEventCheckDone = 6;

constexpr auto kStartOk = 0;
constexpr auto kStartRunning = 2;

constexpr auto kMaxLogLines = 400;
constexpr auto kRetryDelay = crl::time(2000);
constexpr auto kMaxRetryDelay = crl::time(60000);
constexpr auto kMaxRetryShift = 5;
constexpr auto kMainDc = 2;
constexpr auto kSecretLength = 32;

// Check runs pass their generation with this bit set, so the events of a
// check and of a proxy run are told apart
constexpr auto kCheckFlag = quint64(1) << 62;

constexpr auto kLoopback = "127.0.0.1";
constexpr auto kAnyAddress = "0.0.0.0";
constexpr auto kLibraryName = "gls_proxy.dll";
constexpr auto kSettingsName = "tdata/gls_proxy.json";

// The addresses Flowseal's tg-ws-proxy uses: DC 2 and DC 4 answer
// WebSocket connections on the same address
constexpr auto kDefaultDcAddresses = "1:149.154.175.50\n"
	"2:149.154.167.220\n"
	"3:149.154.175.100\n"
	"4:149.154.167.220\n"
	"5:149.154.171.5";
constexpr auto kAutoDcs = std::array{ 2, 4 };

GlsWsProxy *Current = nullptr;

[[nodiscard]] QString DerivedSecret() {
	const auto seed = (cWorkingDir() + u"tg-ws-proxy"_q).toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(
		seed,
		QCryptographicHash::Md5).toHex());
}

[[nodiscard]] bool IsValidSecret(const QString &secret) {
	static const auto re = QRegularExpression(u"^[0-9a-fA-F]{32}$"_q);
	return re.match(secret).hasMatch();
}

[[nodiscard]] QStringList SplitList(const QString &value) {
	static const auto separators = QRegularExpression(u"[\\s,;]+"_q);
	return value.split(separators, Qt::SkipEmptyParts);
}

[[nodiscard]] QString RouteName(GlsProxyRoute route) {
	switch (route) {
	case GlsProxyRoute::CloudflareFirst: return u"cloudflare"_q;
	case GlsProxyRoute::WebSocketOnly: return u"websocket"_q;
	case GlsProxyRoute::Auto: return u"auto"_q;
	}
	Unexpected("Route in RouteName.");
}

[[nodiscard]] GlsProxyRoute RouteFromName(const QString &name) {
	return (name == u"cloudflare"_q)
		? GlsProxyRoute::CloudflareFirst
		: (name == u"websocket"_q)
		? GlsProxyRoute::WebSocketOnly
		: GlsProxyRoute::Auto;
}

[[nodiscard]] GlsProxySettings Normalized(GlsProxySettings settings) {
	if (settings.port <= 0 || settings.port > 65535) {
		settings.port = GlsWsProxy::kDefaultPort;
	}
	if (settings.poolSize < 0 || settings.poolSize > GlsWsProxy::kMaxPoolSize) {
		settings.poolSize = GlsWsProxy::kDefaultPoolSize;
	}
	if (!IsValidSecret(settings.secret)) {
		settings.secret = QString();
	}
	return settings;
}

} // namespace

GlsWsProxy::GlsWsProxy()
: _retryTimer([=] { launch(); }) {
	Current = this;
	_settings.port = kDefaultPort;
	_settings.poolSize = kDefaultPoolSize;
}

GlsWsProxy::~GlsWsProxy() {
	stopRunning();
	if (Current == this) {
		Current = nullptr;
	}
}

GlsWsProxy *GlsWsProxy::Instance() {
	return Current;
}

QString GlsWsProxy::DefaultDcAddresses() {
	return QString::fromLatin1(kDefaultDcAddresses);
}

void GlsWsProxy::start(not_null<SettingsProxy*> settings) {
	_proxySettings = settings;
	loadSettings();
	if (!loadLibrary()) {
		setState(GlsProxyState::Missing);
		return;
	}
	const auto proxy = proxyData();
	registerProxy(proxy);
	if (settings->isSystem() || settings->selected() == proxy) {
		settings->setSelected(proxy);
		settings->setSettings(MTP::ProxyData::Settings::Enabled);
	}

	_connection = settings->connectionTypeValue(
	) | rpl::map([=] {
		return settings->isDisabled()
			? GlsProxyConnection::Direct
			: settings->isSystem()
			? GlsProxyConnection::System
			: (settings->selected() == proxyData())
			? GlsProxyConnection::BuiltIn
			: GlsProxyConnection::Custom;
	});
	_connection.value(
	) | rpl::on_next([=](GlsProxyConnection connection) {
		if (connection == GlsProxyConnection::BuiltIn) {
			if (state() == GlsProxyState::Stopped) {
				launch();
			}
		} else {
			_retryTimer.cancel();
			stopRunning();
			setState(GlsProxyState::Stopped);
		}
	}, _lifetime);
}

const GlsProxySettings &GlsWsProxy::settings() const {
	return _settings;
}

void GlsWsProxy::updateSettings(GlsProxySettings settings) {
	settings = Normalized(std::move(settings));
	if (_settings == settings) {
		return;
	}
	const auto was = proxyData();
	_settings = std::move(settings);
	saveSettings();
	if (_proxySettings) {
		registerProxy(was);
	}
	if (connection() == GlsProxyConnection::BuiltIn) {
		restart();
	}
}

void GlsWsProxy::regenerateSecret() {
	auto settings = _settings;
	auto bytes = QByteArray(kSecretLength / 2, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	settings.secret = QString::fromLatin1(bytes.toHex());
	updateSettings(std::move(settings));
}

void GlsWsProxy::restart() {
	if (state() == GlsProxyState::Missing) {
		return;
	}
	_retryTimer.cancel();
	_retries = 0;
	stopRunning();
	launch();
}

GlsProxyState GlsWsProxy::state() const {
	return _state.current();
}

rpl::producer<GlsProxyState> GlsWsProxy::stateValue() const {
	return _state.value();
}

QString GlsWsProxy::error() const {
	return _error;
}

crl::time GlsWsProxy::startedAt() const {
	return _startedAt;
}

GlsProxyConnection GlsWsProxy::connection() const {
	return _connection.current();
}

rpl::producer<GlsProxyConnection> GlsWsProxy::connectionValue() const {
	return _connection.value();
}

void GlsWsProxy::setConnection(GlsProxyConnection connection) {
	if (!_proxySettings) {
		return;
	}
	const auto selected = _proxySettings->selected();
	switch (connection) {
	case GlsProxyConnection::BuiltIn:
		Core::App().setCurrentProxy(
			proxyData(),
			MTP::ProxyData::Settings::Enabled);
		break;
	case GlsProxyConnection::Custom: {
		const auto ours = proxyData();
		const auto &list = _proxySettings->list();
		const auto custom = (selected && selected != ours)
			? selected
			: [&] {
				for (const auto &proxy : list) {
					if (proxy != ours) {
						return proxy;
					}
				}
				return MTP::ProxyData();
			}();
		if (!custom) {
			return;
		}
		Core::App().setCurrentProxy(
			custom,
			MTP::ProxyData::Settings::Enabled);
	} break;
	case GlsProxyConnection::System:
		Core::App().setCurrentProxy(
			selected,
			MTP::ProxyData::Settings::System);
		break;
	case GlsProxyConnection::Direct:
		Core::App().setCurrentProxy(
			selected,
			MTP::ProxyData::Settings::Disabled);
		break;
	}
	Core::App().saveSettingsDelayed();
}

void GlsWsProxy::runCheck() {
	if (!_check || _lastCheck.running) {
		return;
	}
	_lastCheck = GlsProxyCheck{ .running = true };
	const auto generation = (++_checkGeneration) | kCheckFlag;
	const auto args = arguments();
	const auto result = _check(
		args.constData(),
		&GlsWsProxy::HandleEvent,
		reinterpret_cast<void*>(quintptr(generation)));
	if (result != kStartOk) {
		_lastCheck.running = false;
		_lastCheck.done = true;
	}
	_checkUpdates.fire({});
}

const GlsProxyCheck &GlsWsProxy::check() const {
	return _lastCheck;
}

rpl::producer<> GlsWsProxy::checkUpdates() const {
	return _checkUpdates.events();
}

std::optional<GlsProxyCheckResult> GlsWsProxy::fastestRoute() const {
	const auto &results = _lastCheck.results;
	const auto ok = [](const GlsProxyCheckResult &result) {
		return result.route != u"fail"_q;
	};
	for (const auto &result : results) {
		if (result.dc == kMainDc && ok(result)) {
			return result;
		}
	}
	for (const auto &result : results) {
		if (ok(result)) {
			return result;
		}
	}
	return std::nullopt;
}

const std::deque<QString> &GlsWsProxy::log() const {
	return _log;
}

rpl::producer<> GlsWsProxy::logUpdates() const {
	return _logUpdates.events();
}

MTP::ProxyData GlsWsProxy::proxyData() const {
	auto result = MTP::ProxyData();
	result.type = MTP::ProxyData::Type::Mtproto;
	result.host = QString::fromLatin1(kLoopback);
	result.port = _settings.port;
	result.password = u"dd"_q + secret();
	return result;
}

void GlsWsProxy::HandleEvent(void *context, int kind, const char *text) {
	const auto generation = quint64(reinterpret_cast<quintptr>(context));
	auto copy = QString::fromUtf8(text ? text : "");
	crl::on_main([=, text = std::move(copy)] {
		if (const auto instance = Current) {
			instance->handleEvent(generation, kind, text);
		}
	});
}

void GlsWsProxy::handleEvent(
		quint64 generation,
		int kind,
		const QString &text) {
	if (generation & kCheckFlag) {
		if (generation != (_checkGeneration | kCheckFlag)) {
			return;
		} else if (kind == kEventCheck) {
			handleCheckLine(text);
		} else if (kind == kEventCheckDone) {
			_lastCheck.running = false;
			_lastCheck.done = true;
			_lastCheck.domains = text.toInt();
			_checkUpdates.fire({});
		} else if (kind == kEventError) {
			appendLog(text);
		}
		return;
	} else if (generation != _generation) {
		return;
	}
	switch (kind) {
	case kEventLog:
		appendLog(text);
		break;
	case kEventListening:
		_retries = 0;
		_error = QString();
		_startedAt = crl::now();
		setState(GlsProxyState::Running);
		if (!_lastCheck.done) {
			runCheck();
		}
		break;
	case kEventStopped:
		setState(GlsProxyState::Stopped);
		break;
	case kEventError:
		LOG(("GLS: tg-ws-proxy failed: %1").arg(text));
		appendLog(text);
		_error = text;
		setState(GlsProxyState::Failed);
		scheduleRetry();
		break;
	}
}

void GlsWsProxy::handleCheckLine(const QString &text) {
	const auto parts = text.split(QChar('\t'));
	if (parts.size() < 3) {
		return;
	}
	_lastCheck.results.push_back({
		.dc = parts[0].toInt(),
		.route = parts[1],
		.milliseconds = parts[2].toInt(),
		.details = parts.value(3),
	});
	_checkUpdates.fire({});
}

void GlsWsProxy::appendLog(const QString &line) {
	_log.push_back(line.trimmed());
	while (_log.size() > kMaxLogLines) {
		_log.pop_front();
	}
	_logUpdates.fire({});
}

bool GlsWsProxy::loadLibrary() {
	auto library = std::make_unique<QLibrary>(
		cExeDir() + QString::fromLatin1(kLibraryName));
	if (!library->load()) {
		LOG(("GLS: Could not load %1: %2"
			).arg(QString::fromLatin1(kLibraryName)
			).arg(library->errorString()));
		return false;
	}
	_start = reinterpret_cast<Start>(library->resolve("gls_proxy_start"));
	_stop = reinterpret_cast<Stop>(library->resolve("gls_proxy_stop"));
	_check = reinterpret_cast<Check>(library->resolve("gls_proxy_check"));
	if (!_start || !_stop || !_check) {
		LOG(("GLS: %1 misses its entry points."
			).arg(QString::fromLatin1(kLibraryName)));
		_start = nullptr;
		_stop = nullptr;
		_check = nullptr;
		return false;
	}
	_library = std::move(library);
	return true;
}

void GlsWsProxy::loadSettings() {
	auto file = QFile(cWorkingDir() + QString::fromLatin1(kSettingsName));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto string = [&](const char *key) {
		return object.value(QLatin1String(key)).toString();
	};
	const auto flag = [&](const char *key, bool fallback) {
		return object.value(QLatin1String(key)).toBool(fallback);
	};
	const auto number = [&](const char *key, int fallback) {
		return object.value(QLatin1String(key)).toInt(fallback);
	};
	_settings = Normalized({
		.route = RouteFromName(string("route")),
		.cloudflare = flag("cloudflare", true),
		.cloudflareDomains = string("cloudflareDomains"),
		.workerDomains = string("workerDomains"),
		.http2Media = flag("http2Media", true),
		.noTls = flag("noTls", false),
		.dcAddresses = string("dcAddresses"),
		.frontingDomain = string("frontingDomain"),
		.outboundProxy = string("outboundProxy"),
		.mtprotoFallback = string("mtprotoFallback"),
		.port = number("port", kDefaultPort),
		.secret = string("secret"),
		.poolSize = number("poolSize", kDefaultPoolSize),
		.verbose = flag("verbose", false),
		.shareInNetwork = flag("shareInNetwork", false),
	});
}

void GlsWsProxy::saveSettings() const {
	auto object = QJsonObject();
	object.insert(u"route"_q, RouteName(_settings.route));
	object.insert(u"cloudflare"_q, _settings.cloudflare);
	object.insert(u"cloudflareDomains"_q, _settings.cloudflareDomains);
	object.insert(u"workerDomains"_q, _settings.workerDomains);
	object.insert(u"http2Media"_q, _settings.http2Media);
	object.insert(u"noTls"_q, _settings.noTls);
	object.insert(u"dcAddresses"_q, _settings.dcAddresses);
	object.insert(u"frontingDomain"_q, _settings.frontingDomain);
	object.insert(u"outboundProxy"_q, _settings.outboundProxy);
	object.insert(u"mtprotoFallback"_q, _settings.mtprotoFallback);
	object.insert(u"port"_q, _settings.port);
	object.insert(u"secret"_q, _settings.secret);
	object.insert(u"poolSize"_q, _settings.poolSize);
	object.insert(u"verbose"_q, _settings.verbose);
	object.insert(u"shareInNetwork"_q, _settings.shareInNetwork);

	auto file = QSaveFile(cWorkingDir() + QString::fromLatin1(kSettingsName));
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson());
		file.commit();
	}
}

void GlsWsProxy::registerProxy(const MTP::ProxyData &was) {
	const auto now = proxyData();
	auto &settings = *_proxySettings;
	if (was != now) {
		const auto selected = (settings.selected() == was);
		[[maybe_unused]] const auto removed = settings.removeFromList(was);
		if (selected) {
			settings.setSelected(now);
		}
	}
	if (settings.indexInList(now) < 0) {
		settings.insertToList(0, now);
	}
	if (was != now && settings.selected() == now && settings.isEnabled()) {
		Core::App().setCurrentProxy(now, settings.settings());
	}
	Core::App().saveSettingsDelayed();
}

void GlsWsProxy::launch() {
	if (!_start) {
		return;
	}
	const auto args = arguments();
	const auto generation = ++_generation;
	setState(GlsProxyState::Starting);
	const auto result = _start(
		args.constData(),
		&GlsWsProxy::HandleEvent,
		reinterpret_cast<void*>(quintptr(generation)));
	if (result == kStartRunning) {
		stopRunning();
		_retryTimer.callOnce(kRetryDelay);
	} else if (result != kStartOk) {
		setState(GlsProxyState::Failed);
	}
}

void GlsWsProxy::stopRunning() {
	if (_stop) {
		++_generation;
		_stop(1);
	}
}

void GlsWsProxy::scheduleRetry() {
	const auto shift = std::min(_retries++, kMaxRetryShift);
	_retryTimer.callOnce(std::min(kRetryDelay << shift, kMaxRetryDelay));
}

void GlsWsProxy::setState(GlsProxyState state) {
	if (state != GlsProxyState::Running) {
		_startedAt = 0;
	}
	_state = state;
}

QString GlsWsProxy::secret() const {
	return _settings.secret.isEmpty() ? DerivedSecret() : _settings.secret;
}

QByteArray GlsWsProxy::arguments() const {
	auto list = QStringList{
		u"--host"_q,
		QString::fromLatin1(_settings.shareInNetwork ? kAnyAddress : kLoopback),
		u"--port"_q,
		QString::number(_settings.port),
		u"--secret"_q,
		secret(),
		u"--pool-size"_q,
		QString::number(_settings.poolSize),
	};
	const auto add = [&](const QString &key, const QString &value) {
		list.push_back(key);
		list.push_back(value);
	};
	const auto outbound = _settings.outboundProxy.trimmed();
	if (outbound.isEmpty()) {
		list.push_back(u"--no-outbound-proxy"_q);
	} else {
		add(u"--outbound-proxy"_q, outbound);
	}

	const auto custom = SplitList(_settings.dcAddresses);
	const auto addresses = custom.isEmpty()
		? SplitList(DefaultDcAddresses())
		: custom;
	const auto webSocketOnly = (_settings.route == GlsProxyRoute::WebSocketOnly);
	for (const auto &address : addresses) {
		const auto dc = address.section(QChar(':'), 0, 0).toInt();
		const auto automatic = ranges::contains(kAutoDcs, dc);
		if (!custom.isEmpty() || webSocketOnly || automatic) {
			add(u"--dc-ip"_q, address);
		}
	}
	if (!webSocketOnly) {
		const auto domains = SplitList(_settings.cloudflareDomains);
		if (_settings.cloudflare) {
			if (domains.isEmpty()) {
				list.push_back(u"--default-domains"_q);
			} else {
				add(u"--cf-domain"_q, domains.join(QChar(',')));
			}
		}
		const auto workers = SplitList(_settings.workerDomains);
		if (!workers.isEmpty()) {
			add(u"--cf-worker-domain"_q, workers.join(QChar(',')));
		}
		if (_settings.route == GlsProxyRoute::CloudflareFirst) {
			list.push_back(u"--cf-priority"_q);
		}
		if (!_settings.http2Media) {
			list.push_back(u"--no-h2"_q);
		}
		if (_settings.noTls) {
			list.push_back(u"--cf-disable-tls"_q);
		}
	}
	const auto fronting = _settings.frontingDomain.trimmed();
	if (!fronting.isEmpty()) {
		add(u"--fronting-domain"_q, fronting);
	}
	for (const auto &proxy : SplitList(_settings.mtprotoFallback)) {
		add(u"--mtproto-proxy"_q, proxy);
	}
	if (_settings.verbose) {
		list.push_back(u"--verbose"_q);
	}
	for (auto &value : list) {
		value.remove(QChar('"'));
		if (value.contains(QChar(' '))) {
			value = QChar('"') + value + QChar('"');
		}
	}
	return list.join(QChar(' ')).toUtf8();
}

} // namespace Core
