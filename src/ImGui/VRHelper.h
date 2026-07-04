#pragma once

#include <functional>
#include <span>
#include <string>

// Bridges PhotoMode's ImGui output to the ImGuiVRHelper plugin so the menu renders
// inside the headset and receives controller input on the VR build. On SE/AE these
// are no-ops / a plain DX11 present, leaving the flat-screen path unchanged.
namespace ImGui::Renderer::VR
{
	// kPostPostLoad handshake with the ImGuiVRHelper plugin.
	void Connect();

	// Pump VR controller input (laser cursor, trigger, thumbstick) into ImGui while shown.
	// PhotoMode requests and holds helper focus for the whole session (RequestFocus on
	// activate, ReleaseFocus on exit — see Manager.cpp), so it owns the interactive panel
	// throughout rather than competing with other clients per-frame. Call before ImGui::NewFrame.
	void PumpInput(bool a_shown);

	// Per-frame output: blit to the VR panel when connected, else the normal
	// in-game ImGui_ImplDX11 present. Call after ImGui::Render().
	void RenderFrame();

	bool IsConnected();

	// Exact pixel size of the helper's interactive panel this frame, (0,0) when not
	// connected or unavailable. The ImGui canvas (io.DisplaySize) must match this 1:1
	// while presenting to the panel: the stock DX11 backend renders draw data into a
	// DisplaySize-sized viewport anchored at the panel's top-left, while the helper's
	// wand hit-test UV spans the full panel — any mismatch shrinks the content toward
	// (0,0) and skews clicks/the aim dot toward bottom-right. Re-query every frame;
	// the panel can be resized by the user.
	ImVec2 PanelSize();

	// Composition HUD overlay: a second, HUD-mode helper client whose transparent HMD-anchored plane
	// fills the view, so in-scene aids (grid + the selected overlay frame) frame the actual shot (the
	// main panel is a floating quad). Start on photo-mode activate, Stop on exit; Render every active
	// frame (it self-clears).
	void StartHud();
	void StopHud();
	void RenderHud(const std::function<void()>& a_draw);

	// True when the helper has routed in-scene focus to PhotoMode this frame
	// (i.e. the panel is being composited). Always false on flat screen.
	bool HasFocus();

	// True when the wand laser is intersecting PhotoMode's panel this frame. Used to suppress the
	// free-camera thumbstick drive so the stick scrolls the menu instead of flying the play space.
	// Always false on flat screen.
	bool IsPointerInPanel();

	// VR-only off-panel shortcuts. Defined once in VRHelper.cpp and used for both the helper combo
	// registration and the on-screen hints, so a binding lives in a single place. Registered
	// off-panel: edge-triggered once per press, only while the wand is off the panel (on the panel
	// the same buttons drive the UI). Order matches g_defs / ShortcutHints().
	enum class Shortcut : std::size_t
	{
		TakePhoto,
		HideUI,
		FreezeTime,
		NextTab,
		PrevTab,
		Reset,
		Count
	};

	// True once per off-panel press of the shortcut's controller button.
	bool Pressed(Shortcut a_shortcut);

	// Action label + the controller button that triggers it, in Shortcut order. PhotoMode's in-panel
	// controls legend draws these; button is derived from the combo's live keys so it tracks rebinds.
	struct ShortcutHint
	{
		const char* action;
		std::string button;
	};
	std::span<const ShortcutHint> ShortcutHints();

	// Drain pending shortcut edges; call on activate so a pre-activation press doesn't fire.
	void ArmShortcuts();

	// Take/release the helper's interactive overlay for a photo-mode session. Requested
	// once on activate (so the panel persists once the game unpauses) and released on
	// exit (so the helper's picker is free again). No-op on flat / when not connected.
	void RequestFocus();
	void ReleaseFocus();
}
