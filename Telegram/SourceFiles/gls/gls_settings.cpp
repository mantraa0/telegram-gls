/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gls/gls_settings.h"

#include "gls/gls_glass.h"
#include "lang/lang_instance.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "styles/style_background_preview_box.h"
#include "styles/style_edit_peer_members.h"
#include "styles/style_settings.h"
#include "styles/style_settings_local_storage.h"

namespace Gls {
namespace {

constexpr auto kTintStep = 5;

// The fork has no phrases in the cloud language packs
[[nodiscard]] QString Tr(const char *russian, const char *english) {
	return Lang::GetInstance().id().startsWith(u"ru"_q)
		? QString::fromUtf8(russian)
		: QString::fromUtf8(english);
}

void AddSlider(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		int valuesCount,
		Fn<int(int)> convert,
		int current,
		Fn<QString(int)> format,
		Fn<void(int)> changed) {
	const auto row = container->add(
		object_ptr<Ui::FixedHeightWidget>(
			container,
			st::localStorageLimitLabel.font->height),
		st::localStorageLimitLabelMargin);
	const auto name = Ui::CreateChild<Ui::LabelSimple>(
		row,
		st::localStorageLimitLabel,
		title);
	const auto value = Ui::CreateChild<Ui::LabelSimple>(
		row,
		st::localStorageLimitValue);
	rpl::combine(
		row->widthValue(),
		value->widthValue()
	) | rpl::on_next([=](int width, int) {
		name->moveToLeft(0, 0, width);
		value->moveToRight(0, 0, width);
	}, row->lifetime());
	value->setText(format(current));

	const auto slider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::localStorageLimitSlider),
		st::localStorageLimitMargin);
	slider->resize(st::localStorageLimitSlider.seekSize);
	slider->setPseudoDiscrete(
		valuesCount,
		convert,
		current,
		[=](int now) {
			value->setText(format(now));
			changed(now);
		});
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

} // namespace

QString GlassSettingsTitle() {
	return Tr("Жидкое стекло", "Liquid glass");
}

void SetupGlassSettings(not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsPrivacySkip);
	Ui::AddSubsectionTitle(container, rpl::single(GlassSettingsTitle()));

	const auto enabled = AddToggle(
		container,
		Tr("Стеклянные панели", "Glass panels"),
		GlassEnabled(),
		SetGlassEnabled);

	const auto glass = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto inner = glass->entity();
	AddSlider(
		inner,
		Tr("Плотность стекла", "Glass density"),
		(kMaxTint - kMinTint) / kTintStep + 1,
		[](int index) { return kMinTint + index * kTintStep; },
		GlassTint(),
		[](int value) { return QString::number(value) + '%'; },
		SetGlassTint);
	AddSlider(
		inner,
		Tr("Размытие фона", "Background blur"),
		kMaxBlur + 1,
		[](int index) { return index; },
		GlassBlur(),
		[](int value) { return QString::number(value); },
		SetGlassBlur);
	glass->toggleOn(enabled->toggledValue());
	glass->finishAnimating();

	AddToggle(
		container,
		Tr("Отступы между панелями", "Gaps between panels"),
		GlassGaps(),
		SetGlassGaps);
	AddSlider(
		container,
		Tr("Скругление углов", "Corner radius"),
		kMaxRadius / 2 + 1,
		[](int index) { return index * 2; },
		GlassRadiusSetting(),
		[](int value) { return QString::number(value); },
		SetGlassRadius);

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::producer<QString>(rpl::single(Tr(
			"Панели показывают размытые обои чата. Обои меняются "
			"в разделе «Фон для чатов» ниже.",
			"The panels show the blurred chat wallpaper. Change it in "
			"the Chat background section below."))));
}

} // namespace Gls
