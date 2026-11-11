/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

namespace Ui {
class RpWidget;
} // namespace Ui

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

// The shield in the chat list header: its color tells the proxy state,
// a click opens a menu with the main proxy actions
[[nodiscard]] not_null<Ui::RpWidget*> CreateProxyButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// The round floating button of the chat list with the new chat menu
[[nodiscard]] not_null<Ui::RpWidget*> CreateNewChatButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

} // namespace Gls
