/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "mtproto/mtproto_proxy_data.h"

#include <deque>

class QLibrary;

namespace Core {

class SettingsProxy;

enum class GlsProxyRoute {
	Auto,
	CloudflareFirst,
	WebSocketOnly,
};

struct GlsProxySettings {
	GlsProxyRoute route = GlsProxyRoute::Auto;
	bool cloudflare = true;
	QString cloudflareDomains;
	QString workerDomains;
	bool http2Media = true;
	bool noTls = false;
	QString dcAddresses;
	QString frontingDomain;
	QString outboundProxy;
	QString mtprotoFallback;
	int port = 0;
	QString secret;
	int poolSize = 0;
	bool verbose = false;
	bool shareInNetwork = false;

	friend inline bool operator==(
		const GlsProxySettings &,
		const GlsProxySettings &) = default;
};

enum class GlsProxyState {
	Missing,
	Stopped,
	Starting,
	Running,
	Failed,
};

enum class GlsProxyConnection {
	BuiltIn,
	Custom,
	System,
	Direct,
};

struct GlsProxyCheckResult {
	int dc = 0;
	QString route;
	int milliseconds = 0;
	QString details;
};

struct GlsProxyCheck {
	bool running = false;
	bool done = false;
	int domains = 0;
	std::vector<GlsProxyCheckResult> results;
};

// Runs the tg-ws-proxy-rs core from `gls_proxy.dll` inside the application
// and keeps it registered as a proxy of the connection settings
class GlsWsProxy final {
public:
	static constexpr auto kDefaultPort = 21443;
	static constexpr auto kDefaultPoolSize = 4;
	static constexpr auto kMaxPoolSize = 16;
	static constexpr auto kDcCount = 5;

	GlsWsProxy();
	~GlsWsProxy();

	[[nodiscard]] static GlsWsProxy *Instance();

	void start(not_null<SettingsProxy*> settings);

	[[nodiscard]] const GlsProxySettings &settings() const;
	void updateSettings(GlsProxySettings settings);
	void regenerateSecret();
	void restart();

	[[nodiscard]] GlsProxyState state() const;
	[[nodiscard]] rpl::producer<GlsProxyState> stateValue() const;
	[[nodiscard]] QString error() const;
	[[nodiscard]] crl::time startedAt() const;

	[[nodiscard]] GlsProxyConnection connection() const;
	[[nodiscard]] rpl::producer<GlsProxyConnection> connectionValue() const;
	void setConnection(GlsProxyConnection connection);

	void runCheck();
	[[nodiscard]] const GlsProxyCheck &check() const;
	[[nodiscard]] rpl::producer<> checkUpdates() const;
	[[nodiscard]] std::optional<GlsProxyCheckResult> fastestRoute() const;

	[[nodiscard]] const std::deque<QString> &log() const;
	[[nodiscard]] rpl::producer<> logUpdates() const;

	[[nodiscard]] MTP::ProxyData proxyData() const;
	[[nodiscard]] static QString DefaultDcAddresses();

private:
	using Callback = void(*)(void*, int, const char*);
	using Start = int(*)(const char*, Callback, void*);
	using Stop = void(*)(int);
	using Check = int(*)(const char*, Callback, void*);

	static void HandleEvent(void *context, int kind, const char *text);

	void handleEvent(quint64 generation, int kind, const QString &text);
	void handleCheckLine(const QString &text);
	void appendLog(const QString &line);
	bool loadLibrary();
	void loadSettings();
	void saveSettings() const;
	void registerProxy(const MTP::ProxyData &was);
	void launch();
	void stopRunning();
	void scheduleRetry();
	void setState(GlsProxyState state);
	[[nodiscard]] QByteArray arguments() const;
	[[nodiscard]] QString secret() const;

	SettingsProxy *_proxySettings = nullptr;
	GlsProxySettings _settings;
	std::unique_ptr<QLibrary> _library;
	Start _start = nullptr;
	Stop _stop = nullptr;
	Check _check = nullptr;

	rpl::variable<GlsProxyState> _state = GlsProxyState::Stopped;
	rpl::variable<GlsProxyConnection> _connection;
	QString _error;
	crl::time _startedAt = 0;
	quint64 _generation = 0;
	quint64 _checkGeneration = 0;
	int _retries = 0;
	base::Timer _retryTimer;

	GlsProxyCheck _lastCheck;
	rpl::event_stream<> _checkUpdates;
	std::deque<QString> _log;
	rpl::event_stream<> _logUpdates;

	rpl::lifetime _lifetime;

};

} // namespace Core
