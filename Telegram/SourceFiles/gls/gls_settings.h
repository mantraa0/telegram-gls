/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Gls {

// The "Liquid glass" block of the chat settings
void SetupGlassSettings(not_null<Ui::VerticalLayout*> container);

[[nodiscard]] QString GlassSettingsTitle();

} // namespace Gls
