#include "ImGui/VRHelper.h"

#include "ImGuiVRHelperClientSDK.h"
#include "Version.h"

namespace ImGui::Renderer::VR
{
	namespace
	{
		ImGuiVRHelperPluginAPI::Client g_vrClient;

		// Single source of truth for the off-panel shortcuts: drives both combo registration and the
		// in-panel hint legend. Order must match VR::Shortcut. Dominant hand shoots/composes; the off
		// hand handles tab/reset. The dominant face buttons avoid the helper's secondary open chord.
		struct ShortcutDef
		{
			const char*                             action;  // combo label (helper map) + hint label
			ImGuiVRHelperPluginAPI::InputDeviceType hand;
			std::uint32_t                           key;
			const char*                             button;  // hint text for the button
		};
		using K = RE::BSOpenVRControllerDevice::Keys;
		using D = ImGuiVRHelperPluginAPI::InputDeviceType;
		constexpr ShortcutDef g_defs[] = {
			{ "Take Photo", D::Primary, K::kTrigger, "Trigger" },
			{ "Hide UI", D::Primary, K::kXA, "A/X" },
			{ "Freeze Time", D::Primary, K::kBY, "B/Y" },
			{ "Next Tab", D::Secondary, K::kTrigger, "Off Trigger" },
			{ "Prev Tab", D::Secondary, K::kBY, "Off B/Y" },
			{ "Reset", D::Secondary, K::kXA, "Off A/X" },
		};
		static_assert(std::size(g_defs) == static_cast<std::size_t>(Shortcut::Count));

		ImGuiVRHelperPluginAPI::ComboId g_combos[std::size(g_defs)]{};
		ShortcutHint                    g_hints[std::size(g_defs)]{};

		// Register each def as an off-panel combo (the helper gates and lists it) and mirror it into
		// the hint legend.
		void RegisterShortcuts()
		{
			for (std::size_t i = 0; i < std::size(g_defs); ++i) {
				const auto&       d = g_defs[i];
				const std::vector keys{ ImGuiVRHelperPluginAPI::InputCombo(d.hand, d.key) };
				g_combos[i] = g_vrClient.AddCombo(d.action, keys, {}, keys, /*offPanel*/ true);
				g_hints[i] = { d.action, d.button };
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
	bool IsConnected() { return g_vrClient.IsConnected(); }
	bool HasFocus() { return g_vrClient.HasFocus(); }
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
