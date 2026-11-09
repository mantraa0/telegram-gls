/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_proxy_data.h"

class QProcess;

namespace Core {

class SettingsProxy;

class GlsWsProxy final {
public:
	GlsWsProxy();
	~GlsWsProxy();

	void start(SettingsProxy &settings);

	[[nodiscard]] static MTP::ProxyData BuiltInProxy();

private:
	void launch();

	QString _executable;
	QProcess *_process = nullptr;
	int _restarts = 0;

};

} // namespace Core
