#include "ImGui/VRHelper.h"

#include "ImGuiVRHelperClientSDK.h"
#include "Version.h"

namespace ImGui::Renderer::VR
{
	namespace
	{
		ImGuiVRHelperPluginAPI::Client g_vrClient;
	}

	void Connect()
	{
		if (!REL::Module::IsVR()) {
			return;  // flat screen renders ImGui directly; no helper needed
		}
		// RendersOnFocus: an interactive overlay the helper enters/exits via its own shell —
		// no per-mod combo. The helper keeps focus until the user releases it.
		// We deliberately do NOT request kClientFlag_LiveTool: the free camera reads the raw
		// controller state directly (a hardware poll the helper can't suppress) and moves the play
		// space itself, so forwarding the thumbstick to the game is redundant and would let the
		// right stick double as snap-turn. Let the helper fully swallow controller input while
		// focused; we still rely on it for panel rendering, laser/menu input, and focus.
		if (g_vrClient.Connect("PhotoMode", Version::NAME.data(),
				ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus)) {
			logger::info("Connected to ImGuiVRHelper"sv);
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
	void RequestFocus() { g_vrClient.RequestFocus(); }
	void ReleaseFocus() { g_vrClient.ReleaseFocus(); }
}
