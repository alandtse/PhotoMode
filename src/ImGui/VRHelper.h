#pragma once

// Bridges PhotoMode's ImGui output to the ImGuiVRHelper plugin so the menu renders
// inside the headset and receives controller input on the VR build. On SE/AE these
// are no-ops / a plain DX11 present, leaving the flat-screen path unchanged.
namespace ImGui::Renderer::VR
{
	// kPostPostLoad handshake with the ImGuiVRHelper plugin.
	void Connect();

	// Pump VR controller input (laser cursor, trigger, thumbstick) into ImGui while shown.
	// PhotoMode does NOT request or hold helper focus — the helper owns focus arbitration,
	// so its picker stays free to switch clients. Call before ImGui::NewFrame.
	void PumpInput(bool a_shown);

	// Per-frame output: blit to the VR panel when connected, else the normal
	// in-game ImGui_ImplDX11 present. Call after ImGui::Render().
	void RenderFrame();

	bool IsConnected();

	// True when the helper has routed in-scene focus to PhotoMode this frame
	// (i.e. the panel is being composited). Always false on flat screen.
	bool HasFocus();

	// Take/release the helper's interactive overlay for a photo-mode session. Requested
	// once on activate (so the panel persists once the game unpauses) and released on
	// exit (so the helper's picker is free again). No-op on flat / when not connected.
	void RequestFocus();
	void ReleaseFocus();
}
