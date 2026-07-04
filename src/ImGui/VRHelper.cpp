#include "ImGui/VRHelper.h"

#include "ImGuiVRHelperClientSDK.h"
#include "Version.h"

namespace ImGui::Renderer::VR
{
	namespace
	{
		ImGuiVRHelperPluginAPI::Client g_vrClient;
		// Separate HUD-mode client for in-scene composition aids (grid + the selected overlay frame): a
		// transparent HMD-anchored plane drawn over the world view. The main panel is a floating quad, so
		// these can't frame the actual shot when drawn on it.
		ImGuiVRHelperPluginAPI::Client g_hudClient;

		// Single source of truth for the off-panel shortcuts: drives both combo registration and the
		// in-panel hint legend. Order must match VR::Shortcut. Dominant hand shoots/composes; the off
		// hand handles tab/reset. The dominant face buttons avoid the helper's secondary open chord.
		struct ShortcutDef
		{
			const char*                             action;  // combo label (helper map) + legend action
			ImGuiVRHelperPluginAPI::InputDeviceType hand;
			std::uint32_t                           key;
		};
		using K = RE::BSOpenVRControllerDevice::Keys;
		using D = ImGuiVRHelperPluginAPI::InputDeviceType;
		constexpr ShortcutDef g_defs[] = {
			{ "Take Photo", D::Primary, K::kTrigger },
			{ "Hide UI", D::Primary, K::kXA },
			{ "Freeze Time", D::Primary, K::kBY },
			{ "Next Tab", D::Secondary, K::kTrigger },
			{ "Prev Tab", D::Secondary, K::kBY },
			{ "Reset", D::Secondary, K::kXA },
		};
		static_assert(std::size(g_defs) == static_cast<std::size_t>(Shortcut::Count));

		ImGuiVRHelperPluginAPI::ComboId g_combos[std::size(g_defs)]{};
		ShortcutHint                    g_hints[std::size(g_defs)]{};

		// The helper's controller map lets users rebind our combos; persist those rebinds so they
		// survive a restart. Stored as packed device|key ints (InputCombo::Packed) keyed by the action
		// label, in a small dedicated INI (the MCM settings.ini is owned/rewritten by MCM-Helper).
		constexpr const char* kBindsPath = R"(Data\SKSE\Plugins\PhotoMode_VRControls.ini)";

		std::uint32_t LoadBind(const char* a_action, std::uint32_t a_default)
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.LoadFile(kBindsPath);
			return static_cast<std::uint32_t>(ini.GetLongValue("Binds", a_action, static_cast<long>(a_default)));
		}

		void SaveBind(const char* a_action, std::uint32_t a_packed)
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.LoadFile(kBindsPath);  // preserve the other shortcuts' entries
			ini.SetLongValue("Binds", a_action, static_cast<long>(a_packed));
			(void)ini.SaveFile(kBindsPath);
		}

		// Legend label for a bound chord, matching g_defs's style: the off hand gets an "Off " prefix,
		// the button name comes from the helper's shared mapping so it stays consistent with the
		// controller map and tracks rebinds. Empty chord = unbound.
		std::string HintLabel(const ImGuiVRHelperPluginAPI::InputCombo& a_key)
		{
			auto name = ImGuiVRHelperPluginAPI::ButtonName(a_key.GetDevice(), a_key.GetKey());
			return a_key.GetDevice() == D::Secondary ? "Off " + name : name;
		}

		// Register each def as an off-panel combo (the helper gates and lists it), restoring any
		// persisted rebind and labeling the legend from the live keys.
		void RegisterShortcuts()
		{
			using Combo = ImGuiVRHelperPluginAPI::InputCombo;
			for (std::size_t i = 0; i < std::size(g_defs); ++i) {
				const auto&       d = g_defs[i];
				const std::vector defaultKeys{ Combo(d.hand, d.key) };
				const auto        defaultPacked = defaultKeys[0].Packed();
				const auto saved = LoadBind(d.action, defaultPacked);
				// onRebind persists 0 for an unbound chord (see below), but AddCombo requires a non-empty
				// initial chord to even register the combo (0 keys registered = 0 combo id, permanently
				// unrebindable this session -- the helper has no way to set a registered combo's keys
				// after the fact). Falling back to the default here is the safe bound given that
				// constraint: a saved "unbound" choice reverts to default on restart instead of staying
				// unbound (a real gap; fully fixing it needs the helper SDK to expose a way to clear a
				// registered combo's keys), but never registers the garbage Combo(device=0, key=0) a
				// naive reconstruction of the unbound sentinel would produce -- IsValid() catches that
				// (and any other malformed saved value) the same way it catches the deliberate case.
				const Combo       reconstructed{ static_cast<D>(saved >> 16), saved & 0xFFFFu };
				const std::vector keys{ (saved == defaultPacked || !reconstructed.IsValid()) ? defaultKeys[0] : reconstructed };
				// On rebind: persist the chord and refresh the legend so it shows the live binding.
				auto onRebind = [i](const Combo* a_keys, std::size_t a_n) {
					SaveBind(g_defs[i].action, a_n > 0 ? a_keys[0].Packed() : 0u);
					g_hints[i].button = a_n > 0 ? HintLabel(a_keys[0]) : "(unbound)";
				};
				g_combos[i] = g_vrClient.AddCombo(d.action, keys, onRebind, defaultKeys, /*offPanel*/ true);
				g_hints[i] = { d.action, HintLabel(keys[0]) };
			}
		}
	}

	void Connect()
	{
		if (!REL::Module::IsVR()) {
			return;  // flat screen renders ImGui directly; no helper needed
		}
		// RendersOnFocus: an interactive overlay the helper enters/exits via its own shell; it keeps
		// focus until the user releases it. We don't request kClientFlag_LiveTool (locomotion
		// passthrough) -- the free camera flies the play space itself from the panel UI and off-panel
		// shortcuts, so there's no game locomotion to forward.
		if (g_vrClient.Connect("PhotoMode", Version::NAME.data(),
				ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus)) {
			logger::info("Connected to ImGuiVRHelper"sv);
			RegisterShortcuts();
		} else {
			logger::warn("ImGuiVRHelper not found; the photo mode UI will not render in-headset"sv);
		}
	}

	void PumpInput(bool a_shown) { g_vrClient.PumpInput(a_shown); }
	// When not connected (flat screen / helper absent) RenderFrame() falls back to the
	// normal ImGui_ImplDX11 present, so the desktop path is unchanged.
	void RenderFrame() { g_vrClient.RenderFrame(); }

	// Register/tear down the composition HUD overlay for a photo session, so it exists only while
	// active (no stale grid/overlay lingering in the view after exit).
	void StartHud()
	{
		if (REL::Module::IsVR()) {
			g_hudClient.Connect("PhotoMode HUD", Version::NAME.data(), ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);
		}
	}
	void StopHud()
	{
		g_hudClient.ShutdownHud();
		g_hudClient.Disconnect();
	}
	// Render composition aids into the HUD plane (a transparent, HMD-anchored full-FOV quad). a_draw
	// runs in the helper's private HUD ImGui context; call every active frame so it stays fresh / clears.
	void RenderHud(const std::function<void()>& a_draw)
	{
		if (!g_hudClient.IsConnected()) {
			return;
		}
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}
		const auto device = reinterpret_cast<ID3D11Device*>(RENDERER_DATA(renderer).forwarder);
		const auto context = reinterpret_cast<ID3D11DeviceContext*>(RENDERER_DATA(renderer).context);
		const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
		g_hudClient.RenderHud(device, context, ImVec2(static_cast<float>(screen.width), static_cast<float>(screen.height)), a_draw);
	}

	bool IsConnected() { return g_vrClient.IsConnected(); }
	bool HasFocus() { return g_vrClient.HasFocus(); }
	bool IsPointerInPanel() { return g_vrClient.IsPointerInPanel(); }
	// Edge-triggered once per press, via the helper's combo system. Registered off-panel, so Fired()
	// already returns false while the wand is on the panel.
	bool Pressed(Shortcut a_shortcut) { return g_vrClient.Fired(g_combos[static_cast<std::size_t>(a_shortcut)]); }

	std::span<const ShortcutHint> ShortcutHints() { return g_hints; }

	void ArmShortcuts()
	{
		// Drain any combo edges from before activation so entering photo mode doesn't immediately
		// fire a shortcut left over from a press during normal play.
		for (const auto id : g_combos) {
			g_vrClient.Fired(id);
		}
	}
	void RequestFocus() { g_vrClient.RequestFocus(); }
	void ReleaseFocus() { g_vrClient.ReleaseFocus(); }
}
