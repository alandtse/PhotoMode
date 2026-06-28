#include "Manager.h"

#include "Hotkeys.h"
#include "ImGui/IconsFonts.h"
#include "ImGui/Styles.h"
#include "ImGui/VRHelper.h"
#include "ImGui/Widgets.h"
#include "PlayerClone.h"
#include "Screenshots/Manager.h"

#include "Input.h"

namespace PhotoMode
{
	namespace
	{
		PlayerClone g_photoClone;  // VR-only photographable stand-in for the player

		// Pristine PlayerWorldNode transform captured at activation, restored on exit so the camera
		// rig returns exactly to the player (the play-space drive offsets it during the session).
		RE::NiPoint3  g_vrPlaySpaceOrigin{};
		RE::NiMatrix3 g_vrPlaySpaceOriginRotate{};
		bool          g_vrPlaySpaceCaptured = false;

		// Close any open game menu that pauses the game so the VR free camera can run.
		// The helper's in-scene overlay isn't a game menu, so it stays composited.
		void CloseBlockingMenus()
		{
			const auto ui = RE::UI::GetSingleton();
			const auto msgQueue = RE::UIMessageQueue::GetSingleton();
			if (!ui || !msgQueue) {
				return;
			}
			for (const auto& [name, entry] : ui->menuMap) {
				if (entry.menu && entry.menu->PausesGame()) {
					msgQueue->AddMessage(name, RE::UI_MESSAGE_TYPE::kHide, nullptr);
				}
			}
		}

		// VR keeps the camera locked to its HMD-driven kVR state (the flat FreeCameraState never
		// installs — an engine bug), so a "free camera" moves the play space: translating/rotating
		// PlayerWorldNode flies the whole HMD rig through the world. Facing comes from the live camera
		// rotation (the game's HMD node is frozen by freezeTime). Left stick = horizontal along that
		// facing; right stick X = yaw the view (compose shots); right stick Y = altitude. Sticks read
		// raw — the helper suppresses controller events while focused.
		void DriveVRCamera()
		{
			const auto inputMgr = RE::BSInputDeviceManager::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto vrNodes = player ? player->GetVRNodeData() : nullptr;
			const auto worldNode = vrNodes ? vrNodes->PlayerWorldNode : nullptr;
			if (!worldNode || !inputMgr) {
				return;
			}

			// Left stick = movement, right stick = aim (rotate / altitude).
			const auto moveCtrl = static_cast<RE::BSOpenVRControllerDevice*>(inputMgr->GetVRControllerLeft());
			const auto aimCtrl = static_cast<RE::BSOpenVRControllerDevice*>(inputMgr->GetVRControllerRight());
			if (!moveCtrl || !aimCtrl) {
				return;
			}
			// Deadzone each axis independently: a single active axis must not let sub-deadzone rest
			// noise on the others bleed into slow drift.
			constexpr float dz = 0.15f;
			const auto      deadzone = [](float v) { return std::abs(v) <= dz ? 0.0f : v; };
			const auto      moveStick = moveCtrl->GetThumbstick();
			const auto      aimStick = aimCtrl->GetThumbstick();
			const float     lx = deadzone(moveStick.x);
			const float     ly = deadzone(moveStick.y);
			const float     rx = deadzone(aimStick.x);
			const float     ry = deadzone(aimStick.y);
			if (lx == 0.0f && ly == 0.0f && rx == 0.0f && ry == 0.0f) {
				return;
			}

			// Live HMD facing from the actual VR render camera, updated every frame for rendering (the
			// game's HMD node is frozen by freezeTime). WorldRootCamera() is robust across cell types --
			// the world-root's first child is only the camera in open worldspaces, not towns/interiors.
			RE::NiPoint3 forward{ 0.0f, 1.0f, 0.0f };
			RE::NiPoint3 right{ 1.0f, 0.0f, 0.0f };
			const auto   renderCam = RE::Main::WorldRootCamera();
			if (renderCam) {
				const auto& m = renderCam->world.rotate;
				// WorldRootCamera is an NiCamera in a view basis: col0 look, col1 up, col2 right
				// (verified in-headset). Use col0 as the gaze; col1 is up (tilts with pitch), col2 right.
				forward = { m.entry[0][0], m.entry[1][0], m.entry[2][0] };
			}
			const auto flatten = [](RE::NiPoint3& v) {
				const float len = std::sqrt(v.x * v.x + v.y * v.y);
				v.z = 0.0f;
				if (len > 1e-4f) {
					v.x /= len;
					v.y /= len;
				}
			};
			flatten(forward);
			// Derive strafe from the flattened look so it is always 90 degrees to the right in the
			// horizontal plane. Reading the camera's own X column instead drifts with HMD pitch/roll,
			// so left/right flips with orientation while forward/back stays correct.
			right = { forward.y, -forward.x, 0.0f };

			const float  dt = RE::BSTimer::GetSingleton()->realTimeDelta;
			const float  speed = FreeCamera::translateSpeed * 100.0f;
			RE::NiPoint3 delta = forward * (ly * speed * dt) + right * (lx * speed * dt);
			delta.z += ry * speed * dt;  // right stick Y = altitude
			worldNode->local.translate += delta;

			// right stick X = yaw the rig about the HMD so the view rotates in place. The rig
			// translate and the pivot are both in world-aligned space (PlayerWorldNode's parent is
			// the scene root at the origin), so orbiting local.translate around the camera world
			// position keeps the HMD fixed while turning -- not a sweep about the world origin.
			if (std::abs(rx) > dz) {
				RE::NiMatrix3 yaw;
				yaw.MakeRotation(rx * 1.5f * dt, RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
				const RE::NiPoint3 pivot = renderCam ? renderCam->world.translate : worldNode->local.translate;
				worldNode->local.translate = pivot + yaw * (worldNode->local.translate - pivot);
				worldNode->local.rotate = yaw * worldNode->local.rotate;
			}

			RE::NiUpdateData updateData{};
			worldNode->Update(updateData);
		}

		// Restore the play space to where it was at activation, so exiting doesn't leave the HMD
		// desynced from the player position.
		void ResetVRPlaySpace()
		{
			if (!g_vrPlaySpaceCaptured) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto vrNodes = player ? player->GetVRNodeData() : nullptr;
			if (const auto worldNode = vrNodes ? vrNodes->PlayerWorldNode : nullptr) {
				// Unify the play space back onto the player: undo the fly offset (translate) and the
				// compose yaw (rotate) accumulated during the session.
				worldNode->local.translate = g_vrPlaySpaceOrigin;
				worldNode->local.rotate = g_vrPlaySpaceOriginRotate;
				RE::NiUpdateData updateData{};
				worldNode->Update(updateData);
			}
			g_vrPlaySpaceCaptured = false;
		}
	}

	void Manager::Register()
	{
		tweenMenuInstalled = GetModuleHandle(L"TweenMenuOverhaul") != nullptr;
		improvedCameraInstalled = GetModuleHandle(L"ImprovedCameraSE.dll") != nullptr;
		skyrimSoulsInstalled = GetModuleHandle(L"SkyrimSoulsRE.dll") != nullptr;

		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(this);
		logger::info("Registered for menu open/close event");

		if (tweenMenuInstalled) {
			SKSE::GetModCallbackEventSource()->AddEventSink(this);
			logger::info("Registered for mod callback event");
		}
	}

	void Manager::LoadMCMSettings(const CSimpleIniA& a_ini)
	{
		freeCameraSpeed = static_cast<float>(a_ini.GetDoubleValue("Settings", "fFreeCameraTranslationSpeed", freeCameraSpeed));
		freezeTimeOnStart = a_ini.GetBoolValue("Settings", "bFreezeTimeOnStart", freezeTimeOnStart);
		openFromPauseMenu = a_ini.GetBoolValue("Settings", "bOpenFromPauseMenu", openFromPauseMenu);
	}

	bool Manager::IsValid()
	{
		static constexpr std::array badMenus{
			RE::MainMenu::MENU_NAME,
			RE::MistMenu::MENU_NAME,
			RE::LoadingMenu::MENU_NAME,
			RE::FaderMenu::MENU_NAME,
			"LootMenu"sv,
			"CustomMenu"sv
		};

		const auto UI = RE::UI::GetSingleton();
		if (!UI || std::ranges::any_of(badMenus, [&](const auto& menuName) { return UI->IsMenuOpen(menuName); })) {
			return false;
		}

		if (!GetValidControlMapContext() || RE::MenuControls::GetSingleton()->InBeastForm() || RE::VATS::GetSingleton()->mode == RE::VATS::VATS_MODE::kKillCam) {
			return false;
		}

		return true;
	}

	bool Manager::GetValidControlMapContext()
	{
		const auto* controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap) {
			return false;
		}

		switch (CONTROLMAP_DATA(controlMap).contextPriorityStack.back()) {
		case RE::UserEvents::INPUT_CONTEXT_ID::kGameplay:
		case RE::UserEvents::INPUT_CONTEXT_ID::kTFCMode:
		case RE::UserEvents::INPUT_CONTEXT_ID::kConsole:
		case RE::UserEvents::INPUT_CONTEXT_ID::kCursor:
			return true;
		default:
			return false;
		}
	}

	bool Manager::ShouldBlockInput() const
	{
		return blockInputToPhotoMode;
	}

	bool Manager::IsActive() const
	{
		return activated;
	}

	bool Manager::IsHidden() const
	{
		return hiddenUI;
	}

	void Manager::ToggleUI()
	{
		hiddenUI = !hiddenUI;

		const auto UI = RE::UI::GetSingleton();
		UI->ShowMenus(!UI->IsShowingMenus());
		RE::PlaySound("UIMenuFocus");

		if (!hiddenUI) {
			restoreLastFocusID = true;
		}
	}

	void Manager::Activate()
	{
		RE::PlaySound("UIMenuOK");

		cameraTab.GetOriginalState();
		timeTab.GetOriginalState();

		const auto player = RE::PlayerCharacter::GetSingleton();
		characterTab.emplace(player->GetFormID(), Character(player));
		cachedCharacter = player;

		filterTab.GetOriginalState();

		const auto pcCamera = RE::PlayerCamera::GetSingleton();
		originalcameraState = pcCamera->currentState ? pcCamera->currentState->id : RE::CameraState::kThirdPerson;

		menusAlreadyHidden = !RE::UI::GetSingleton()->IsShowingMenus();
		// Flat: inherit the HUD-hidden state (opened with menus off -> keep the panel off). VR: the
		// helper overlay isn't a game menu, so IsShowingMenus is normally false — that must not hide
		// the photo panel.
		hiddenUI = menusAlreadyHidden && !REL::Module::IsVR();

		// disable saving
		PLAYER_GAMESTATE(RE::PlayerCharacter::GetSingleton()).byCharGenFlag.set(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);

		// Flat screen uses the engine free camera. VR can't: its ToggleFreeCameraMode never installs
		// the state (engine bug) and the camera stays HMD-locked (kVR), so VR leaves the camera
		// state untouched and flies the play space instead (see DriveVRCamera).
		if (!REL::Module::IsVR() && originalcameraState != RE::CameraState::kFree) {
			pcCamera->ToggleFreeCameraMode(false);
			// clear the plane lock so forward follows the aim pitch instead of staying plane-locked
			if (const auto freeState = skyrim_cast<RE::FreeCameraState*>(pcCamera->currentState.get())) {
				freeState->lockToZPlane = false;
			}
		}

		// disable controls
		TogglePlayerControls(false);

		// hide the VR first-person body so it doesn't ride the free camera, and place a
		// static clone at the player's spot so the character can still be photographed.
		// Spawn before time is frozen below so the clone can load its 3D.
		HideVRFirstPersonBody(true);
		if (REL::Module::IsVR()) {
			// Snapshot the rig's resting transform so exit can unify the play space back onto the
			// player. Flying mutates PlayerWorldNode->local; restoring this on exit removes the offset.
			if (const auto vrNodes = player ? player->GetVRNodeData() : nullptr) {
				if (const auto worldNode = vrNodes->PlayerWorldNode) {
					g_vrPlaySpaceOrigin = worldNode->local.translate;
					g_vrPlaySpaceOriginRotate = worldNode->local.rotate;
					g_vrPlaySpaceCaptured = true;
				}
			}
			g_photoClone.Spawn();
			CloseBlockingMenus();                 // drop the pause so the free camera runs
			ImGui::Renderer::VR::RequestFocus();  // own the interactive panel for this session
			// The Character tab edits the photographed subject; in VR that's the clone, not the
			// hidden player.
			if (const auto clone = g_photoClone.GetClone()) {
				characterTab.insert_or_assign(clone->GetFormID(), Character(clone));
				cachedCharacter = clone;
			}
		}

		// apply mcm settings
		FreeCamera::translateSpeed = freeCameraSpeed;
		pendingVRFreeze = false;
		vrFreezeDelay = 0;
		if (freezeTimeOnStart) {
			if (REL::Module::IsVR()) {
				pendingVRFreeze = true;  // defer until the clone has streamed in (OnFrameUpdate)
			} else {
				SetTimeFrozen(true);
			}
		}

		// load default screenshot keys
		// keybindings can change?
		MANAGER(Input)->LoadDefaultKeys();

		// refresh style
		ImGui::Styles::GetSingleton()->RefreshStyle();

		activated = true;
		if (activeGlobal) {
			activeGlobal->value = 1.0f;
		}
	}

	void Manager::TogglePlayerControls(bool a_enable)
	{
		RE::ControlMap::GetSingleton()->ToggleControls(controlFlags, a_enable, true);

		if (const auto pcControls = RE::PlayerControls::GetSingleton()) {
			pcControls->readyWeaponHandler->SetInputEventHandlingEnabled(a_enable);
			pcControls->sneakHandler->SetInputEventHandlingEnabled(a_enable);
			pcControls->autoMoveHandler->SetInputEventHandlingEnabled(a_enable);
			pcControls->shoutHandler->SetInputEventHandlingEnabled(a_enable);
			pcControls->attackBlockHandler->SetInputEventHandlingEnabled(a_enable);
		}
	}

	void Manager::HideVRFirstPersonBody(bool a_hide)
	{
		// The VR free camera carries the first-person arms/hands with it, which clutters
		// every shot. Cull the first-person 3D so the flown camera sees a clean scene.
		// Re-applied each frame because the engine/VR body mods re-show it.
		if (!REL::Module::IsVR()) {
			return;
		}
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			if (const auto firstPerson3D = player->Get3D(true)) {
				firstPerson3D->SetAppCulled(a_hide);
			}
		}
	}

	bool Manager::OnFrameUpdate()
	{
		// Flat screen exits the session when a blocking menu or invalid control context appears.
		// VR ignores this gate entirely: the helper owns enter/exit via focus, and the free camera
		// runs an intentionally non-standard control context that IsValid() rejects — bailing here
		// would skip all the per-frame VR work (clone pose/effects, deferred freeze, body hide).
		if (!REL::Module::IsVR() && !IsValid()) {
			Deactivate();
			return false;
		}

		// disable controls
		if (ImGui::GetIO().WantTextInput) {
			if (!allowTextInput) {
				allowTextInput = true;
				RE::ControlMap::GetSingleton()->AllowTextInput(true);
			}
		} else if (allowTextInput) {
			allowTextInput = false;
			RE::ControlMap::GetSingleton()->AllowTextInput(false);
		}
		TogglePlayerControls(false);

		// keep the VR first-person body hidden (the engine/body mods re-show it each frame)
		HideVRFirstPersonBody(true);

		// match the clone to the player's frozen entry pose once its 3D has streamed in (no-op
		// after it applies once)
		if (REL::Module::IsVR()) {
			g_photoClone.ApplyPose();

			// Fly the play space (VR has no usable detached camera — see DriveVRCamera).
			DriveVRCamera();

			// Deferred freeze: only freeze once the clone has streamed in (freezeTime stalls its
			// 3D load), with a frame-count fallback in case it never readies.
			if (pendingVRFreeze && (g_photoClone.IsReady() || ++vrFreezeDelay > 90)) {
				SetTimeFrozen(true);
				pendingVRFreeze = false;
			}
		}

		timeTab.OnFrameUpdate();

		return true;
	}

	bool Manager::HasOverlay() const
	{
		return overlaysTab.HasOverlay();
	}

	void Manager::Deactivate()
	{
		pendingVRFreeze = false;
		Revert(true);

		//reset characters
		characterTab.clear();
		cachedCharacter = nullptr;

		// reset camera
		// Flat-only: VR never entered the engine free camera (it flies the play space), so toggling
		// here would erroneously enter free-cam on exit.
		if (!REL::Module::IsVR() && originalcameraState != RE::CameraState::kFree) {
			RE::PlayerCamera::GetSingleton()->ToggleFreeCameraMode(false);
		}

		// reset controls
		allowTextInput = false;
		RE::ControlMap::GetSingleton()->AllowTextInput(false);
		TogglePlayerControls(true);

		// restore the VR first-person body, remove the photo clone, and hand the helper's
		// overlay back so its picker is free again
		HideVRFirstPersonBody(false);
		g_photoClone.Despawn();
		if (REL::Module::IsVR()) {
			ResetVRPlaySpace();  // restore the play-space node; never touches data.location (no save risk)
			ImGui::Renderer::VR::ReleaseFocus();
		}

		// allow saving
		PLAYER_GAMESTATE(RE::PlayerCharacter::GetSingleton()).byCharGenFlag.reset(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);

		// reset variables
		hiddenUI = false;

		noItemsFocused = false;
		restoreLastFocusID = false;
		lastFocusedID = 0;

		updateKeyboardFocus = false;

		MANAGER(Input)->ToggleCursor(false);
		MANAGER(Input)->ResetInputDevices();

		activated = false;
		if (activeGlobal) {
			activeGlobal->value = 0.0f;
		}

		RE::PlaySound("UIMenuCancel");
	}

	void Manager::ToggleActive()
	{
		if (!IsActive()) {
			if (IsValid() && !ShouldBlockInput()) {
				Activate();
			}
		} else {
			if (!ImGui::GetIO().WantTextInput && !ShouldBlockInput()) {
				Deactivate();
			}
		}
	}

	bool Manager::IsTimeFrozen() const
	{
		return MAIN_DATA(RE::Main::GetSingleton()).freezeTime;
	}

	void Manager::SetTimeFrozen(bool a_frozen)
	{
		// freezeTime halts the main update loop. In VR the panel and the free-fly camera are driven
		// from the always-firing StopTimer render hook (which reads the controllers raw), so they
		// keep running while frozen; the caller defers enabling this until the clone has streamed in,
		// since the freeze would otherwise stall its 3D load.
		MAIN_DATA(RE::Main::GetSingleton()).freezeTime = a_frozen;
	}

	void Manager::Revert(bool a_deactivate)
	{
		const std::int32_t tabIndex = a_deactivate ? -1 : currentTab;

		// Camera
		if (tabIndex == -1 || tabIndex == kCamera) {
			cameraTab.RevertState(a_deactivate);
			if (!a_deactivate) {
				FreeCamera::translateSpeed = freeCameraSpeed;
			}
			revertENB = true;
		}
		// Time/Weather
		if (tabIndex == -1 || tabIndex == kTime) {
			timeTab.RevertState();
		}

		// Character
		if (tabIndex == kCharacter) {
			if (cachedCharacter) {
				characterTab[cachedCharacter->GetFormID()].RevertState();
			}
		} else if (tabIndex == -1) {
			std::ranges::for_each(characterTab, [](auto& data) {
				data.second.RevertState();
			});
		}

		// Filters
		if (tabIndex == -1 || tabIndex == kFilters) {
			filterTab.RevertState(tabIndex == -1);
		}
		// Overlays
		if (tabIndex == -1 || tabIndex == kOverlays) {
			overlaysTab.RevertOverlays();
		}

		if (a_deactivate) {
			// reset UI
			if ((!menusAlreadyHidden || hiddenUI) && !RE::UI::GetSingleton()->IsShowingMenus()) {
				RE::UI::GetSingleton()->ShowMenus(true);
			}
			resetWindow = true;
			resetPlayerTabs = true;
		} else {
			RE::PlaySound("UIMenuOK");

			const auto notification = std::format("{}", resetAll ? "$PM_ResetNotifAll"_T : TRANSLATE(tabResetNotifs[currentTab]));
			RE::SendHUDMessage::ShowHUDMessage(notification.c_str());

			if (resetAll) {
				resetAll = false;
			}
		}
	}

	void Manager::QuitOnEscape()
	{
		if (IsHidden() || noItemsFocused) {
			Deactivate();
			RE::PlaySound("UIMenuCancel");
		}
	}

	bool Manager::GetResetAll() const
	{
		return resetAll;
	}

	void Manager::DoResetAll()
	{
		resetAll = true;
	}

	void Manager::NavigateTab(bool a_left)
	{
		constexpr auto tabsSizeInt32 = static_cast<uint32_t>(tabs.size());
		if (a_left) {
			currentTab = (currentTab - static_cast<uint32_t>(1) + tabsSizeInt32) % tabsSizeInt32;
		} else {
			currentTab = (currentTab + static_cast<uint32_t>(1)) % tabsSizeInt32;
		}
		UpdateKeyboardFocus();
		RE::PlaySound("UIJournalTabsSD");
	}

	void Manager::UpdateKeyboardFocus()
	{
		updateKeyboardFocus = true;
	}

	float Manager::GetViewRoll(const float a_fallback) const
	{
		return IsActive() ? cameraTab.GetViewRoll() : a_fallback;
	}

	float Manager::GetViewRoll() const
	{
		return cameraTab.GetViewRoll();
	}

	void Manager::SetViewRoll(float a_value)
	{
		cameraTab.SetViewRoll(a_value);
	}

	void Manager::TryOpenFromTweenMenu()
	{
		if (openFromTweenMenu) {
			SKSE::GetTaskInterface()->AddTask([this]() {
				this->Activate();
				this->openFromTweenMenu = false;
			});
		}
	}

	void Manager::OnDataLoad()
	{
		overlaysTab.LoadOverlays();

		activeGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("PhotoMode_IsActive");
		resetRootIdle = RE::TESForm::LookupByEditorID<RE::TESIdleForm>("ResetRoot");
	}

	void Manager::OnGameLoad()
	{
		// The form DB is rebuilt and the player rig re-established, so the cached clone base would
		// dangle and the play-space snapshot would restore stale coordinates. Drop both.
		g_photoClone.Invalidate();
		g_vrPlaySpaceCaptured = false;
	}

	std::pair<ImGui::Texture*, float> Manager::GetOverlay() const
	{
		return overlaysTab.GetCurrentOverlay();
	}

	bool Manager::IsCursorHoveringOverWindow() const
	{
		return isCursorHoveringOverWindow;
	}

	void Manager::Draw()
	{
		ImGui::SetNextWindowPos(ImGui::GetNativeViewportPos());
		ImGui::SetNextWindowSize(ImGui::GetNativeViewportSize());

		ImGui::Begin("##Main", nullptr, ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration);
		{
			if (!IsHidden()) {
				overlaysTab.DrawOverlays();
				CameraGrid::Draw();
				DrawBar();
				DrawControls();

				// VR has no Escape key, so offer an on-panel way out of the session.
				if (REL::Module::IsVR()) {
					const auto pos = ImGui::GetNativeViewportPos();
					const auto size = ImGui::GetNativeViewportSize();
					ImGui::SetCursorScreenPos({ pos.x + size.x - 220.0f, pos.y + 40.0f });
					if (ImGui::Button("Exit Photo Mode", { 190.0f, 48.0f })) {
						ToggleActive();
					}
				}
			}
		}
		ImGui::End();
	}

	void Manager::DrawOverlays()
	{
		ImGui::SetNextWindowPos(ImGui::GetNativeViewportPos());
		ImGui::SetNextWindowSize(ImGui::GetNativeViewportSize());

		ImGui::Begin("##MainOverlay", nullptr, ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration);
		{
			// render hierachy
			overlaysTab.DrawOverlays();
		}
		ImGui::End();
	}

	void Manager::DrawControls()
	{
		const static auto center = ImGui::GetNativeViewportCenter();
		const static auto size = ImGui::GetNativeViewportSize();

		const static auto third_width = size.x / 3;
		const static auto third_height = size.y / 3;

		constexpr auto windowFlags = ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDecoration;

		ImGui::SetNextWindowPos(ImVec2(center.x + third_width, center.y + third_height * 0.8f), ImGuiCond_Always, ImVec2(0.5, 0.5));
		ImGui::SetNextWindowSize(ImVec2(size.x / 3.25f, size.y / 3.125f));

		bool navigateWithMouse = MANAGER(Input)->CanNavigateWithMouse();

		ImGui::Begin("$PM_Title_Menu"_T, nullptr, windowFlags);
		{
			ImGui::ExtendWindowPastBorder();

			if (resetWindow) {
				currentTab = kCamera;
			}

			// console already covers menu
			if (blockInputToPhotoMode) {
				ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, ImGui::GetStyle().Alpha);
			}

			ImGui::BeginDisabled(blockInputToPhotoMode);
			{
				// Q [Tab Tab Tab Tab Tab] E
				ImGui::BeginGroup();
				{
					const auto buttonSize = ImGui::ButtonIcon(MANAGER(Hotkeys)->PreviousTabKey());
					ImGui::SameLine();

					const float tabWidth = (ImGui::GetContentRegionAvail().x - (buttonSize.x + ImGui::GetStyle().ItemSpacing.x * tabs.size())) / tabs.size();

					ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4());
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4());

					for (std::int32_t i = 0; i < tabs.size(); ++i) {
						bool activeTab = (currentTab == i) || hoveredTabs[i] == true;
						if (!activeTab) {
							ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
						} else {
							ImGui::PushFont(MANAGER(IconFont)->GetLargeFont());
						}
						ImGui::Button(tabIcons[i], ImVec2(tabWidth, ImGui::GetFrameHeightWithSpacing()));
						if (ImGui::IsItemClicked() && currentTab != i) {
							currentTab = i;
						}
						hoveredTabs[i] = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
						if (!activeTab) {
							ImGui::PopStyleColor();
						} else {
							ImGui::PopFont();
						}
						ImGui::SameLine();
					}
					ImGui::PopStyleColor(3);
					ImGui::PopItemFlag();

					ImGui::SameLine();
					ImGui::ButtonIcon(MANAGER(Hotkeys)->NextTabKey());
				}
				ImGui::EndGroup();

				//		CAMERA
				// ----------------
				ImGui::CenteredText(currentTab != TAB_TYPE::kCharacter ? TRANSLATE(tabs[currentTab]) : characterTab[cachedCharacter->GetFormID()].GetName());
				ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ImGui::GetUserStyleVar(ImGui::USER_STYLE::kSeparatorThickness));

				// content
				ImGui::SetNextWindowBgAlpha(0.0f);  // child bg color is added ontop of window
				ImGui::BeginChild("##PhotoModeChild", ImVec2(0, 0), ImGuiChildFlags_None, windowFlags);
				{
					ImGui::Spacing();

					if (restoreLastFocusID) {
						navigateWithMouse ? ImGui::SetHoveredID(lastHoveredID) : ImGui::SetFocusID(lastFocusedID, ImGui::GetCurrentWindow());

						restoreLastFocusID = false;
					} else if (updateKeyboardFocus) {
						if (currentTab == TAB_TYPE::kCharacter) {
							resetPlayerTabs = true;
						}
						navigateWithMouse ? ImGui::SetItemDefaultFocus() : ImGui::SetKeyboardFocusHere();
						updateKeyboardFocus = false;
					}

					switch (currentTab) {
					case TAB_TYPE::kCamera:
						{
							if (resetWindow) {
								navigateWithMouse ? ImGui::SetItemDefaultFocus() : ImGui::SetKeyboardFocusHere();
								resetWindow = false;
							}
							cameraTab.Draw();
						}
						break;
					case TAB_TYPE::kTime:
						timeTab.Draw();
						break;
					case TAB_TYPE::kCharacter:
						{
							const auto consoleRef = RE::Console::GetSelectedRef();
							if (!consoleRef || !consoleRef->Is(RE::FormType::ActorCharacter) || consoleRef->IsDisabled() || consoleRef->IsDeleted() || !consoleRef->Is3DLoaded()) {
								prevCachedCharacter = cachedCharacter;
								cachedCharacter = RE::PlayerCharacter::GetSingleton();
							} else {
								prevCachedCharacter = cachedCharacter;
								cachedCharacter = consoleRef->As<RE::Actor>();
								if (!characterTab.contains(cachedCharacter->GetFormID())) {
									characterTab.emplace(cachedCharacter->GetFormID(), Character(cachedCharacter));
								}
							}

							if (cachedCharacter != prevCachedCharacter) {
								resetPlayerTabs = true;
							}

							characterTab[cachedCharacter->GetFormID()].Draw(resetPlayerTabs, navigateWithMouse);

							if (resetPlayerTabs) {
								resetPlayerTabs = false;
							}
						}
						break;
					case TAB_TYPE::kFilters:
						filterTab.Draw();
						break;
					case TAB_TYPE::kOverlays:
						overlaysTab.Draw();
						break;
					default:
						break;
					}

					noItemsFocused = navigateWithMouse ?
					                     (!ImGui::IsAnyItemHovered() || !isCursorHoveringOverWindow) :
					                     (!ImGui::IsAnyItemFocused() || !ImGui::IsWindowFocused());
					lastFocusedID = ImGui::GetFocusID();
					lastHoveredID = ImGui::GetHoveredID();
				}
				ImGui::EndChild();
			}
			ImGui::EndDisabled();

			if (blockInputToPhotoMode) {
				ImGui::PopStyleVar();
			}

			if (navigateWithMouse) {
				UpdateMouseHoveringOverWindow();
			}
		}
		ImGui::End();
	}

	void Manager::DrawBar() const
	{
		const static auto center = ImGui::GetNativeViewportCenter();
		const static auto size = ImGui::GetNativeViewportSize();
		const static auto offsetY = size.y / 25.0f;

		ImGui::SetNextWindowPos(ImVec2(center.x, size.y - offsetY), ImGuiCond_Always, ImVec2(0.5, 0.5));

		bool canNavigateWithMouse = MANAGER(Input)->CanNavigateWithMouse();

		ImGui::Begin("##Bar", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);  // same offset as control window
		{
			ImGui::ExtendWindowPastBorder();

			const static auto takePhotoLabel = "$PM_TAKEPHOTO"_T;
			const static auto toggleMenusLabel = "$PM_TOGGLEMENUS"_T;
			const auto        resetLabel = GetResetAll() ? "$PM_RESET_ALL"_T : "$PM_RESET"_T;
			const static auto freezeTimeLabel = "$PM_FREEZETIME"_T;
			const static auto panCameraLabel = "$PM_PAN_CAMERA"_T;

			const auto& takePhotoIcon = MANAGER(Hotkeys)->TakePhotoIcon();
			const auto& toggleMenusIcon = MANAGER(Hotkeys)->ToggleMenusIcon();
			const auto& resetIcon = MANAGER(Hotkeys)->ResetIcon();
			const auto& freezeTimeIcon = MANAGER(Hotkeys)->FreezeTimeIcon();
			const auto& panCameraIcon = MANAGER(Hotkeys)->PanCameraIcon();

			// const static auto togglePMLabel = "$PM_EXIT"_T;
			// const auto& togglePMIcons = MANAGER(Hotkeys)->TogglePhotoModeIcons();

			// calc total elements width
			const ImGuiStyle& style = ImGui::GetStyle();

			float width = 0.0f;

			const auto calc_width = [&](const IconFont::IconTexture* a_icon, const char* a_textLabel, bool a_sameLine = true) {
				width += a_icon->size.x;
				width += style.ItemSpacing.x;
				width += ImGui::CalcTextSize(a_textLabel).x;
				if (a_sameLine) {
					width += style.ItemSpacing.x;
				}
			};

			if (canNavigateWithMouse) {
				calc_width(panCameraIcon, panCameraLabel);
			}
			calc_width(takePhotoIcon, takePhotoLabel);
			calc_width(toggleMenusIcon, toggleMenusLabel);
			calc_width(freezeTimeIcon, freezeTimeLabel);
			calc_width(resetIcon, resetLabel, false);

			/*for (const auto& icon : togglePMIcons) {
				width += icon->size.x;
			}
			width += style.ItemSpacing.x;
			width += ImGui::CalcTextSize(togglePMLabel).x;*/

			// align at center
			ImGui::AlignForWidth(width);

			// draw
			constexpr auto draw_button = [](const IconFont::IconTexture* a_icon, const char* a_textLabel, bool a_sameLine = true) {
				ImGui::ButtonIconWithLabel(a_textLabel, a_icon, true);
				if (a_sameLine) {
					ImGui::SameLine();
				}
			};

			if (canNavigateWithMouse) {
				draw_button(panCameraIcon, panCameraLabel);
			}
			draw_button(takePhotoIcon, takePhotoLabel);
			draw_button(toggleMenusIcon, toggleMenusLabel);
			draw_button(freezeTimeIcon, freezeTimeLabel);
			draw_button(resetIcon, resetLabel, false);

			// ImGui::ButtonIconWithLabel(togglePMLabel, togglePMIcons, true);
		}
		ImGui::End();
	}

	bool Manager::SetupJournalMenu() const
	{
		const auto menu = RE::UI::GetSingleton()->GetMenu<RE::JournalMenu>(RE::JournalMenu::MENU_NAME);
		const auto view = menu ? JOURNALMENU_DATA(menu).systemTab.view : nullptr;

		RE::GFxValue page;
		if (!view || !view->GetVariable(&page, "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc")) {
			return false;
		}

		// in case someone packed the files into a BSA
		static bool dearDiaryExists = RE::BSResourceNiBinaryStream(R"(interface\deardiary_dm\config.txt)").good() || RE::BSResourceNiBinaryStream(R"(interface\deardiary\config.txt)").good();

		// Dear Diary SetShowMod function is broken af, need to do it manually
		if (dearDiaryExists) {
			RE::GFxValue categoryList;
			if (page.GetMember("CategoryList", &categoryList)) {
				RE::GFxValue entryList;
				if (categoryList.GetMember("entryList", &entryList)) {
					std::vector<std::string> elements;

					entryList.VisitMembers([&](const char*, const RE::GFxValue& a_value) {
						RE::GFxValue textVal;
						a_value.GetMember("text", &textVal);
						elements.push_back(textVal.GetString());
					});

					RE::GFxValue showModMenu;
					if (page.GetMember("_showModMenu", &showModMenu) && !showModMenu.GetBool()) {
						page.SetMember("_showModMenu", true);
					} else {
						std::erase(elements, "$MOD MANAGER");
					}

					auto index = std::ranges::contains(elements, "$QUICKSAVE") ? 3 : 2;
					elements.insert(elements.begin() + index, "$PM_Title_Menu");

					entryList.ClearElements();
					for (auto& element : elements) {
						RE::GFxValue entry;
						view->CreateObject(&entry);
						entry.SetMember("text", element.c_str());
						entryList.PushBack(entry);
					}

					categoryList.Invoke("InvalidateData");

					return true;
				}
			}

		} else {
			RE::GFxValue showModMenu;
			if (page.GetMember("_showModMenu", &showModMenu) && !showModMenu.GetBool()) {
				std::array<RE::GFxValue, 1> args;
				args[0] = true;
				if (!page.Invoke("SetShowMod", nullptr, args.data(), args.size())) {
					return false;
				}
			}

			RE::GFxValue categoryList;
			if (page.GetMember("CategoryList", &categoryList)) {
				RE::GFxValue entryList;
				if (categoryList.GetMember("entryList", &entryList)) {
					std::optional<std::uint32_t> modMenuIndex = std::nullopt;

					std::uint32_t index = 0;
					std::string   text;
					entryList.VisitMembers([&](const char*, const RE::GFxValue& a_value) {
						RE::GFxValue textVal;
						a_value.GetMember("text", &textVal);
						if (text = textVal.GetString(); text == "$MOD MANAGER") {
							modMenuIndex = index;
						}
						index++;
					});

					if (modMenuIndex) {
						RE::GFxValue entry;
						view->CreateObject(&entry);
						entry.SetMember("text", "$PM_Title_Menu");

						entryList.SetElement(*modMenuIndex, entry);
						categoryList.Invoke("InvalidateData");

						return true;
					}
				}
			}
		}

		return false;
	}

	void Manager::UpdateMouseHoveringOverWindow()
	{
		constexpr float buffer = 50.0f;
		auto            mousePos = ImGui::GetMousePos();
		auto            winPos = ImGui::GetWindowPos();
		auto            winSize = ImGui::GetWindowSize();

		isCursorHoveringOverWindow =
			mousePos.x >= winPos.x - buffer &&
			mousePos.x <= winPos.x + winSize.x + buffer &&
			mousePos.y >= winPos.y - buffer &&
			mousePos.y <= winPos.y + winSize.y + buffer;
	}

	EventResult Manager::ProcessEvent(const RE::MenuOpenCloseEvent* a_evn, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		if (!a_evn) {
			return EventResult::kContinue;
		}

		if (a_evn->menuName == RE::Console::MENU_NAME) {
			blockInputToPhotoMode = a_evn->opening;
			if (a_evn->opening) {
				if (IsActive() && IsHidden()) {
					ToggleUI();
				}
			} else if (IsActive() && MANAGER(Input)->DoNavigateWithMouse()) {
				Input::Manager::ToggleCursor(true);
			}
		} else if (a_evn->menuName == RE::TweenMenu::MENU_NAME) {
			if (!a_evn->opening) {
				TryOpenFromTweenMenu();
			}
		} else if (a_evn->opening) {
			if (a_evn->menuName == RE::JournalMenu::MENU_NAME) {
				if (openFromPauseMenu) {
					openFromPauseMenu = SetupJournalMenu();
				}
			} else if (a_evn->menuName == RE::ModManagerMenu::MENU_NAME) {
				if (RE::UI::GetSingleton()->IsMenuOpen(RE::JournalMenu::MENU_NAME) && openFromPauseMenu) {
					const auto msgQueue = RE::UIMessageQueue::GetSingleton();

					msgQueue->AddMessage(RE::ModManagerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					msgQueue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);

					Activate();
				}
			}
		}

		return EventResult::kContinue;
	}

	EventResult Manager::ProcessEvent(const SKSE::ModCallbackEvent* a_evn, RE::BSTEventSource<SKSE::ModCallbackEvent>*)
	{
		if (a_evn && a_evn->eventName == "OpenTween_PhotoMode") {
			openFromTweenMenu = true;
			if (skyrimSoulsInstalled) {
				Activate();
				openFromTweenMenu = false;
			}
		}

		return EventResult::kContinue;
	}
}
