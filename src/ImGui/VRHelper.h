#pragma once

// Bridges PhotoMode's ImGui output to the ImGuiVRHelper plugin so the menu renders
// inside the headset and receives controller input on the VR build. On SE/AE these
// are no-ops / a plain DX11 present, leaving the flat-screen path unchanged.
namespace ImGui::Renderer::VR
{
	// kPostPostLoad handshake with the ImGuiVRHelper plugin.
	void Connect();

	// Reconcile panel focus and pump VR controller input (laser cursor, trigger,
	// thumbstick) into ImGui. Call before ImGui::NewFrame.
	void Update(bool a_menuOpen);

	// Per-frame output: blit to the VR panel when connected, else the normal
	// in-game ImGui_ImplDX11 present. Call after ImGui::Render().
	void RenderFrame();

	bool IsConnected();
}
