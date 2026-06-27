#include "ImGui/VRHelper.h"

#ifdef SKYRIMVR
#	include "ImGuiVRHelperClientSDK.h"
#	include "Version.h"
#else
#	include <imgui.h>
#	include <imgui_impl_dx11.h>
#endif

namespace ImGui::Renderer::VR
{
#ifdef SKYRIMVR
	namespace
	{
		ImGuiVRHelperPluginAPI::Client g_vrClient;
	}

	void Connect()
	{
		// RendersOnFocus: an interactive, navigable menu (not a passive HUD layer).
		if (g_vrClient.Connect("PhotoMode", Version::NAME.data(), ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus)) {
			logger::info("Connected to ImGuiVRHelper"sv);
		} else {
			logger::warn("ImGuiVRHelper not found; the photo mode UI will not render in-headset"sv);
		}
	}

	void Update(bool a_menuOpen) { g_vrClient.Update(a_menuOpen); }
	void RenderFrame() { g_vrClient.RenderFrame(); }
	bool IsConnected() { return g_vrClient.IsConnected(); }
#else
	void Connect() {}
	void Update(bool) {}
	void RenderFrame() { ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); }
	bool IsConnected() { return false; }
#endif
}
