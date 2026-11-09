/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/gls_ws_proxy.h"

#include "core/core_settings_proxy.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>

#ifdef Q_OS_WIN
#include "base/platform/win/base_windows_h.h"
#endif // Q_OS_WIN

namespace Core {
namespace {

constexpr auto kProxyPort = 21443;
constexpr auto kMaxRestarts = 5;
constexpr auto kProxyHost = "127.0.0.1";
constexpr auto kExecutableName = "tg-ws-proxy.exe";

[[nodiscard]] QString ProxySecret() {
	const auto seed = (cWorkingDir() + u"tg-ws-proxy"_q).toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(
		seed,
		QCryptographicHash::Md5).toHex());
}

} // namespace

GlsWsProxy::GlsWsProxy()
: _executable(cExeDir() + QString::fromLatin1(kExecutableName)) {
}

GlsWsProxy::~GlsWsProxy() {
	if (const auto process = base::take(_process)) {
		process->disconnect();
		process->kill();
		process->waitForFinished(1000);
		delete process;
	}
}

MTP::ProxyData GlsWsProxy::BuiltInProxy() {
	auto result = MTP::ProxyData();
	result.type = MTP::ProxyData::Type::Mtproto;
	result.host = QString::fromLatin1(kProxyHost);
	result.port = kProxyPort;
	result.password = u"dd"_q + ProxySecret();
	return result;
}

void GlsWsProxy::start(SettingsProxy &settings) {
	if (!QFileInfo::exists(_executable)) {
		return;
	}
	const auto proxy = BuiltInProxy();
	if (settings.indexInList(proxy) < 0) {
		settings.insertToList(0, proxy);
	}
	if (settings.isSystem() || settings.selected() == proxy) {
		settings.setSelected(proxy);
		settings.setSettings(MTP::ProxyData::Settings::Enabled);
	}
	launch();
}

void GlsWsProxy::launch() {
	if (!_process) {
		_process = new QProcess();
		_process->setProgram(_executable);
		_process->setArguments({
			u"--host"_q,
			QString::fromLatin1(kProxyHost),
			u"--port"_q,
			QString::number(kProxyPort),
			u"--secret"_q,
			ProxySecret(),
			u"--quiet"_q,
		});
		_process->setStandardOutputFile(QProcess::nullDevice());
		_process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
		_process->setCreateProcessArgumentsModifier([](
				QProcess::CreateProcessArguments *args) {
			args->flags |= CREATE_NO_WINDOW;
		});
#endif // Q_OS_WIN
		QObject::connect(
			_process,
			qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
			_process,
			[=] {
				if (_restarts++ < kMaxRestarts) {
					LOG(("GLS: tg-ws-proxy stopped, restarting."));
					launch();
				}
			});
	}
	_process->start();
}

} // namespace Core
