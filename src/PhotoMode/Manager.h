#pragma once

#include "ImGui/IconsFontAwesome6.h"

#include "Tabs/Camera.h"
#include "Tabs/Character.h"
#include "Tabs/Filters.h"
#include "Tabs/Overlays.h"
#include "Tabs/Time.h"

namespace PhotoMode
{
	class Manager :
		public REX::Singleton<Manager>,
		public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
		public RE::BSTEventSink<SKSE::ModCallbackEvent>
	{
	public:
		void Register();
		void LoadMCMSettings(const CSimpleIniA& a_ini);

		bool IsValid();
		bool GetValidControlMapContext();

		bool               ShouldBlockInput() const;
		[[nodiscard]] bool IsActive() const;
		void               Activate();
		void               Deactivate();
		void               ToggleActive();
		void               Revert(bool a_deactivate = false);
		void               QuitOnEscape();

		// Freeze the world for a still shot via Main::freezeTime. That flag also halts the main update
		// loop, so in VR the panel and free-cam are driven from the always-firing StopTimer hook to
		// survive it, and the freeze is deferred until the photo clone has streamed in.
		[[nodiscard]] bool IsTimeFrozen() const;
		void               SetTimeFrozen(bool a_frozen);

		bool GetResetAll() const;
		void DoResetAll();

		[[nodiscard]] bool IsHidden() const;
		void               ToggleUI();

		void NavigateTab(bool a_left);
		void UpdateKeyboardFocus();

		[[nodiscard]] float GetViewRoll(float a_fallback) const;
		[[nodiscard]] float GetViewRoll() const;
		void                SetViewRoll(float a_value);

		void TryOpenFromTweenMenu();

		void Draw();
		void DrawOverlays();
		bool OnFrameUpdate();
		bool HasOverlay() const;

		void OnDataLoad();
		// Reset state invalidated by a save load / new game: the VR clone's runtime base (the form
		// DB is rebuilt) and the captured play-space snapshot (the rig is re-established).
		void                              OnGameLoad();
		std::pair<ImGui::Texture*, float> GetOverlay() const;

		bool IsCursorHoveringOverWindow() const;

	private:
		enum TAB_TYPE : std::int8_t
		{
			kCamera,
			kTime,
			kCharacter,
			kFilters,
			kOverlays
		};

		// kMenu | kActivate | kJumping
		static constexpr auto controlFlags = static_cast<RE::ControlMap::UEFlag>(1036);

		// VR defers the freeze-on-start until the photo clone has streamed in (a freeze stalls its
		// 3D load). Set on activate, cleared once applied or on deactivate.
		bool          pendingVRFreeze{ false };
		std::uint32_t vrFreezeDelay{ 0 };  // fallback frame counter if the clone never readies

		static constexpr std::array tabs = {
			"$PM_Camera",
			"$PM_TimeWeather",
			"$PM_Player",
			"$PM_Filters",
			"$PM_Overlays"
		};
		static constexpr std::array tabIcons = {
			ICON_FA_CAMERA,
			ICON_FA_CLOCK,
			ICON_FA_PERSON,
			ICON_FA_CIRCLE_HALF_STROKE,
			ICON_FA_IMAGE
		};
		static constexpr std::array tabResetNotifs = { "$PM_ResetNotifCamera", "$PM_ResetNotifTime", "$PM_ResetNotifPlayer", "$PM_ResetNotifFilters", "$PM_ResetNotifOverlays" };

		static void        TogglePlayerControls(bool a_enable);
		static void        HideVRFirstPersonBody(bool a_hide);  // declutter the free-cam view in VR
		void               DrawControls();
		void               DrawBar() const;
		[[nodiscard]] bool SetupJournalMenu() const;
		void               UpdateMouseHoveringOverWindow();

		EventResult ProcessEvent(const RE::MenuOpenCloseEvent* a_evn, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		EventResult ProcessEvent(const SKSE::ModCallbackEvent* a_evn, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override;

		// members
		bool activated{ false };
		bool hiddenUI{ false };
		bool revertENB{ false };
		bool blockInputToPhotoMode{ false };

		std::int32_t                  previousTab{ kCamera };
		std::int32_t                  currentTab{ kCamera };
		std::array<bool, tabs.size()> hoveredTabs{};

		Camera cameraTab;
		Time   timeTab;

		NodeMap<RE::FormID, Character> characterTab;
		RE::Actor*                     cachedCharacter{ nullptr };
		RE::Actor*                     prevCachedCharacter{ nullptr };

		Filters  filterTab;
		Overlays overlaysTab;

		bool updateKeyboardFocus{ false };

		RE::CameraState originalcameraState{ RE::CameraState::kThirdPerson };

		bool resetWindow{ true };
		bool resetPlayerTabs{ true };
		bool resetAll{ false };

		bool improvedCameraInstalled{ false };
		bool tweenMenuInstalled{ false };
		bool skyrimSoulsInstalled{ false };
		bool openFromTweenMenu{};

		bool menusAlreadyHidden{ false };
		bool allowTextInput{ false };

		bool    noItemsFocused{ false };
		ImGuiID lastFocusedID{ 0 };
		ImGuiID lastHoveredID{ 0 };
		bool    restoreLastFocusID{ false };

		float freeCameraSpeed{ 4.0f };
		bool  freezeTimeOnStart{ true };
		bool  openFromPauseMenu{ true };

		bool isCursorHoveringOverWindow{ false };

		RE::TESGlobal* activeGlobal{ nullptr };
	};
}
