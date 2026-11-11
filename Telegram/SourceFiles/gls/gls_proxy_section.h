/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Gls {

// The settings screen of the built-in tg-ws-proxy
[[nodiscard]] Settings::Type ProxySectionId();

// Runs a connection check and shows its results as they come
void ShowProxyCheckBox(not_null<Window::SessionController*> controller);

// A short state of the connection for settings rows, e.g. "Connected"
[[nodiscard]] rpl::producer<QString> ProxyStatusValue();

// Whether the built-in proxy works now, for status colors
[[nodiscard]] rpl::producer<bool> ProxyHealthyValue();

} // namespace Gls
