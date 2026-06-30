#include "Renderer.h"
#include "IconsFonts.h"
#include "Styles.h"
#include "VRHelper.h"

#include "PhotoMode/Manager.h"

namespace ImGui::Renderer
{
	float GetResolutionScale()
	{
		static auto height = RE::BSGraphics::Renderer::GetScreenSize().height;
		return DisplayTweaks::borderlessUpscale ? DisplayTweaks::resolutionScale : height / 1080.0f;
	}

	void LoadSettings(const CSimpleIniA& a_ini)
	{
		DisplayTweaks::resolutionScale = static_cast<float>(a_ini.GetDoubleValue("Render", "ResolutionScale", static_cast<double>(DisplayTweaks::resolutionScale)));
		DisplayTweaks::borderlessUpscale = a_ini.GetBoolValue("Render", "BorderlessUpscale", DisplayTweaks::borderlessUpscale);
	}

	struct WndProc
	{
		static LRESULT thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
		{
			auto& io = ImGui::GetIO();
			if (uMsg == WM_KILLFOCUS) {
				io.ClearInputKeys();
			}

			return func(hWnd, uMsg, wParam, lParam);
		}
		static inline WNDPROC func;
	};

	struct CreateD3DAndSwapChain
	{
		static void thunk()
		{
			func();

			if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				const auto swapChain = reinterpret_cast<IDXGISwapChain*>(RENDERER_DATA(renderer).renderWindows[0].swapChain);
				if (!swapChain) {
					logger::error("couldn't find swapChain");
					return;
				}

				DXGI_SWAP_CHAIN_DESC desc{};
				if (FAILED(swapChain->GetDesc(std::addressof(desc)))) {
					logger::error("IDXGISwapChain::GetDesc failed.");
					return;
				}

				const auto device = reinterpret_cast<ID3D11Device*>(RENDERER_DATA(renderer).forwarder);
				const auto context = reinterpret_cast<ID3D11DeviceContext*>(RENDERER_DATA(renderer).context);

				logger::info("Initializing ImGui..."sv);

				ImGui::CreateContext();

				auto& io = ImGui::GetIO();
				io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
				io.IniFilename = nullptr;

				if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
					logger::error("ImGui initialization failed (Win32)");
					return;
				}
				if (!ImGui_ImplDX11_Init(device, context)) {
					logger::error("ImGui initialization failed (DX11)"sv);
					return;
				}

				MANAGER(IconFont)->LoadIcons();

				logger::info("ImGui initialized.");

				initialized.store(true);

				WndProc::func = reinterpret_cast<WNDPROC>(
					SetWindowLongPtrA(
						desc.OutputWindow,
						GWLP_WNDPROC,
						reinterpret_cast<LONG_PTR>(WndProc::thunk)));
				if (!WndProc::func) {
					logger::error("SetWindowLongPtrA failed!");
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Build one ImGui frame at the game's real resolution and present it (to the VR helper when
	// connected, else the desktop swapchain). a_draw fills the frame.
	template <class F>
	static void RenderImGuiFrame(F&& a_draw)
	{
		ImGui_ImplDX11_NewFrame();
		SKSE::ImGui_ImplWin32_NewFrame();
		{
			// trick imgui into rendering at game's real resolution (ie. if upscaled with Display Tweaks)
			static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();

			auto& io = ImGui::GetIO();
			io.DisplaySize.x = static_cast<float>(screenSize.width);
			io.DisplaySize.y = static_cast<float>(screenSize.height);
		}
		ImGui::NewFrame();
		{
			// disable windowing
			GImGui->NavWindowingTarget = nullptr;

			a_draw();
		}
		// In VR the helper points a wand laser at the panel and sets MouseDrawCursor when it
		// intersects, but ImGui's default software cursor is tiny/invisible at panel scale. Draw a
		// legible pointer at the laser position ourselves (foreground draw list blits with the panel).
		if (REL::Module::IsVR()) {
			auto& io = ImGui::GetIO();
			if (io.MouseDrawCursor) {
				io.MouseDrawCursor = false;
				const auto p = io.MousePos;
				auto*      dl = ImGui::GetForegroundDrawList();
				dl->AddCircleFilled(p, 7.0f, IM_COL32(255, 255, 255, 240));
				dl->AddCircle(p, 8.5f, IM_COL32(0, 0, 0, 240), 0, 2.5f);
			}
		}
		ImGui::EndFrame();
		ImGui::Render();
		VR::RenderFrame();
	}

	// IMenu::PostDisplay
	struct PostDisplay
	{
		static void thunk(RE::IMenu* a_menu)
		{
			// Skip if Imgui is not loaded
			if (!initialized.load()) {
				return func(a_menu);
			}

			// VR renders + pumps input from the always-firing StopTimer hook instead: this HUD
			// hook stops firing once time is frozen, which would freeze/hide the panel.
			if (REL::Module::IsVR()) {
				return func(a_menu);
			}

			const auto photoMode = MANAGER(PhotoMode);
			if (!photoMode->IsActive() || !photoMode->OnFrameUpdate()) {
				return func(a_menu);
			}

			// refresh style
			ImGui::Styles::GetSingleton()->OnStyleRefresh();

			RenderImGuiFrame([&] { photoMode->Draw(); });

			return func(a_menu);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline std::size_t                      idx{ 0x6 };
	};

	struct StopTimer
	{
		static void thunk(std::uint32_t timer)
		{
			func(timer);

			// Skip if Imgui is not loaded
			if (!initialized.load()) {
				return;
			}

			const auto photoMode = MANAGER(PhotoMode);

			if (REL::Module::IsVR()) {
				// Drive enter/exit from this always-rendered hook and bypass the menu-open validity
				// gate — the helper's focus grant (the user picked PhotoMode in its shell,
				// necessarily from a paused menu) is deliberate intent.
				const bool focused = VR::HasFocus();
				if (focused && !photoMode->IsActive()) {
					if (const auto pc = RE::PlayerCharacter::GetSingleton(); pc && pc->Is3DLoaded()) {
						photoMode->Activate();
					}
				} else if (!focused && photoMode->IsActive()) {
					photoMode->Deactivate();
				}

				// Render the MAIN panel here too (PostDisplay stops firing once time is frozen).
				// Pump laser input every frame; run the per-frame update whenever active (not gated
				// by hidden — pose/freeze must still tick), then draw + present when it's shown.
				VR::PumpInput(photoMode->IsActive());
				if (photoMode->IsActive()) {
					// Composition aids (grid + overlay frame) live on their own HMD-anchored HUD plane (the
					// panel is a floating quad that can't frame the shot). Independent of the panel present
					// below, so draw every active frame — it self-clears when aids are off or UI is hidden.
					photoMode->DrawVRHud();
					const bool draw = photoMode->OnFrameUpdate();
					if (draw && !photoMode->IsHidden()) {
						ImGui::Styles::GetSingleton()->OnStyleRefresh();
						RenderImGuiFrame([&] { photoMode->Draw(); });
						return;  // one ImGui present per frame; overlays are mutually exclusive (panel shown)
					}
				}
			}

			if (!photoMode->IsActive() || !photoMode->IsHidden()) {
				return;
			}
			if (photoMode->HasOverlay()) {
				RenderImGuiFrame([&] { photoMode->DrawOverlays(); });
			} else if (REL::Module::IsVR()) {
				// Hidden with nothing to draw: render an empty frame so the VR panel blits fully
				// transparent (clear color alpha 0) and actually disappears, instead of freezing the
				// last menu image on the overlay.
				RenderImGuiFrame([] {});
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(75595, 77226), OFFSET(0x9, 0x275) };  // BSGraphics::InitD3D
		stl::write_thunk_call<CreateD3DAndSwapChain>(target.address());

		REL::Relocation<std::uintptr_t> target2{ RELOCATION_ID(75461, 77246), OFFSET_3(0x9, 0x9, 0x15) };  // BSGraphics::Renderer::End
		stl::write_thunk_call<StopTimer>(target2.address());

		stl::write_vfunc<RE::HUDMenu, PostDisplay>();
	}
}
