/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lang/lang_instance.h"

namespace Gls {

// The fork has no phrases in the cloud language packs, so its texts pick
// Russian or English by the interface language
[[nodiscard]] inline QString Tr(const char *russian, const char *english) {
	return Lang::GetInstance().id().startsWith(u"ru"_q)
		? QString::fromUtf8(russian)
		: QString::fromUtf8(english);
}

} // namespace Gls
