#include "Filters.h"

namespace PhotoMode
{
	void Filters::GetOriginalState()
	{
		imods.InitForms();

		const auto  IMGS = RE::ImageSpaceManager::GetSingleton();
		auto&       imgs = IMAGESPACE_DATA(IMGS);
		if (imgs.currentBaseData) {
			imageSpaceData = *imgs.currentBaseData;
		}
		imgs.overrideBaseData = &imageSpaceData;
	}

	void Filters::RevertState(bool a_fullReset)
	{
		// reset imagespace
		const auto  IMGS = RE::ImageSpaceManager::GetSingleton();
		auto&       imgs = IMAGESPACE_DATA(IMGS);
		if (a_fullReset) {
			imgs.overrideBaseData = nullptr;
		} else if (imgs.overrideBaseData) {
			if (imgs.currentBaseData) {
				imageSpaceData = *imgs.currentBaseData;
			}
			imgs.overrideBaseData = &imageSpaceData;
		}

		// reset imod
		imods.Reset();
		if (imodPlayed) {
			if (currentImod) {
				RE::ImageSpaceModifierInstanceForm::Stop(currentImod);
				currentImod = nullptr;
			}
			imodPlayed = false;
		}
	}

	void Filters::Draw()
	{
		if (const auto& overrideData = IMAGESPACE_DATA(RE::ImageSpaceManager::GetSingleton()).overrideBaseData) {
			ImGui::Slider("$PM_Brightness"_T, &overrideData->cinematic.brightness, 0.0f, 3.0f, nullptr, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
			ImGui::Slider("$PM_Saturation"_T, &overrideData->cinematic.saturation, 0.0f, 3.0f);
			ImGui::Slider("$PM_Contrast"_T, &overrideData->cinematic.contrast, 0.0f, 3.0f);

			ImGui::Slider("$PM_TintAlpha"_T, &overrideData->tint.amount, 0.0f, 1.0f);
			ImGui::Indent();
			{
				ImGui::Slider("$PM_TintRed"_T, &overrideData->tint.color.red, 0.0f, 1.0f);
				ImGui::Slider("$PM_TintBlue"_T, &overrideData->tint.color.blue, 0.0f, 1.0f);
				ImGui::Slider("$PM_TintGreen"_T, &overrideData->tint.color.green, 0.0f, 1.0f);
			}
			ImGui::Unindent();
		} else {
			IMAGESPACE_DATA(RE::ImageSpaceManager::GetSingleton()).overrideBaseData = &imageSpaceData;
		}

		imods.GetFormResultFromCombo([&](const auto& imod) {
			if (currentImod) {
				RE::ImageSpaceModifierInstanceForm::Stop(currentImod);
			}
			RE::ImageSpaceModifierInstanceForm::Trigger(imod, 1.0, nullptr);
			currentImod = imod;
			imodPlayed = true;
		});
	}
}
