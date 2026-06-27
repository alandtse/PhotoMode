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
		// RendersOnFocus: an interactive, navigable menu (not a passive HUD layer).
		if (g_vrClient.Connect("PhotoMode", Version::NAME.data(), ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus)) {
			logger::info("Connected to ImGuiVRHelper"sv);
		} else {
			logger::warn("ImGuiVRHelper not found; the photo mode UI will not render in-headset"sv);
		}
	}

	void Update(bool a_menuOpen) { g_vrClient.Update(a_menuOpen); }
	// When not connected (flat screen / helper absent) RenderFrame() falls back to the
	// normal ImGui_ImplDX11 present, so the desktop path is unchanged.
	void RenderFrame() { g_vrClient.RenderFrame(); }
	bool IsConnected() { return g_vrClient.IsConnected(); }
}
