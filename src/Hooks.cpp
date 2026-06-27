#include "Hooks.h"

#include "Input.h"
#include "PhotoMode/Manager.h"
#include "Screenshots/LoadScreen.h"
#include "Screenshots/Manager.h"

namespace PhotoMode
{
	struct FromEulerAnglesZXY
	{
		static void thunk(RE::NiMatrix3* a_matrix, float a_z, float a_x, float a_y)
		{
			return func(a_matrix, a_z, a_x, MANAGER(PhotoMode)->GetViewRoll(a_y));
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// TESDataHandler idle array is not populated
	struct SetFormEditorID
	{
		static bool thunk(RE::TESIdleForm* a_this, const char* a_str)
		{
			if (!clib_util::string::is_empty(a_str)) {
				if (const std::string_view str(a_str); !str.starts_with("pa_")) {  // paired anims
					cachedIdles.emplace(a_str, a_this);
				}
			}
			return func(a_this, a_str);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline constexpr std::size_t            idx{ 0x33 };
	};

	struct ApplyFootIKErrorFeedback
	{
		static bool thunk(RE::Actor* a_actor)
		{
			if (a_actor && a_actor->IsPlayerRef() && MANAGER(PhotoMode)->IsActive()) {
				return false;
			}
			return func(a_actor);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> getRot{ RELOCATION_ID(49814, 50744), 0x1B };  // FreeCamera::GetRotation
		stl::write_thunk_call<FromEulerAnglesZXY>(getRot.address());

		stl::write_vfunc<RE::TESIdleForm, SetFormEditorID>();

		//REL::Relocation<std::uintptr_t> applyFootIKErrorFeedback{ RELOCATION_ID(42527, 43690) };  // Actor::ApplyFootIKErrorFeedback
		//stl::hook_function_prologue<ApplyFootIKErrorFeedback, 5>(applyFootIKErrorFeedback.address());
	}
}

namespace Screenshot
{
	struct TakeScreenshot
	{
		static void thunk(char const* a_path, RE::BSGraphics::TextureFileFormat a_format)
		{
			bool skipVanillaScreenshot = false;

			if (MANAGER(Input)->IsScreenshotQueued()) {
				skipVanillaScreenshot = MANAGER(Screenshot)->TakeScreenshot();
			}

			if (!skipVanillaScreenshot) {
				func(a_path, a_format);
			}

			MANAGER(Input)->OnScreenshotFinish();

			if (skipVanillaScreenshot) {
				RE::SendHUDMessage::ShowHUDMessage("$PM_ScreenshotNotif"_T);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(35556, 36555), OFFSET(0x48E, 0x454) };  // Main::Swap
		stl::write_thunk_call<TakeScreenshot>(target.address());
	}
}

namespace LoadScreen
{
	struct GetLoadScreenModel
	{
		static RE::TESModelTextureSwap* thunk([[maybe_unused]] RE::TESLoadScreen* a_loadScreen)
		{
			if (const auto screenshotModel = MANAGER(LoadScreen)->LoadScreenshotModel()) {
				return screenshotModel;
			}
			return func(a_loadScreen);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct InitLoadScreen3D
	{
		static void thunk(RE::MistMenu* a_this, float a_scale, const RE::NiPoint3& a_rotateOffset, const RE::NiPoint3& a_translateOffset, const char* a_cameraShotPath)
		{
			if (auto transform = MANAGER(LoadScreen)->GetModelTransform()) {
				func(a_this, transform->scale, transform->rotationalOffset, transform->translateOffset, MANAGER(LoadScreen)->GetCameraShotPath(a_cameraShotPath));
				if (const auto canvas = MISTMENU_DATA(a_this).loadScreenModel ? MISTMENU_DATA(a_this).loadScreenModel->GetObjectByName("Canvas:0") : nullptr) {
					MANAGER(LoadScreen)->ApplyScreenshotTexture(canvas->AsGeometry());
				}
			} else {
				func(a_this, a_scale, a_rotateOffset, a_translateOffset, a_cameraShotPath);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(51048, 51929), OFFSET_3(0x384, 0x27B, 0x394) };
		stl::write_thunk_call<GetLoadScreenModel>(target.address());

		REL::Relocation<std::uintptr_t> target2{ RELOCATION_ID(51454, 52313), OFFSET_3(0x1D1, 0x1C3, 0x210) };
		stl::write_thunk_call<InitLoadScreen3D>(target2.address());
	}
}

namespace VRTFCFix
{
	// `tfc` (toggle free camera) crashes in VR. The VR build of PlayerCamera::ToggleFreeCameraMode
	// (id 49876) emits broken code at +0x48..+0x6b: it never loads a base register for the
	// FreeCameraState, so the three position stores target absolute addresses 0x30/0x34/0x38 and
	// it passes RCX=0 to the following call (FUN_140848880). The FreeCameraState lives at [RBX+0xD8].
	// Reimplement the position writes against [RBX+0xD8] and restore RCX before resuming at +0x6b.
	//
	// Verified in SkyrimVR.exe (1.4.15) @ 0x140876880: position = ([rsp+0x20], [rsp+0x24], [rsp+0x28]).
	struct Patch : Xbyak::CodeGenerator
	{
		explicit Patch(std::uintptr_t a_rtn)
		{
			Xbyak::Label retLab;

			mov(rdi, qword[rbx + 0xd8]);     // rdi = FreeCameraState
			movss(dword[rdi + 0x30], xmm0);  // Position.x = xmm0 (already [rsp+0x20])
			mov(rcx, rdi);                   // restore RCX = FreeCameraState for the call at +0x6b
			movss(xmm1, dword[rsp + 0x24]);
			movss(dword[rdi + 0x34], xmm1);  // Position.y = [rsp+0x24]
			movss(xmm0, dword[rsp + 0x28]);
			movss(dword[rdi + 0x38], xmm0);  // Position.z = [rsp+0x28]

			jmp(ptr[rip + retLab]);
			L(retLab);
			dq(a_rtn);
		}
	};

	void InstallHooks()
	{
		static REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(49876, 0), OFFSET(0x48, 0) };
		static REL::Relocation<std::uintptr_t> resume{ RELOCATION_ID(49876, 0), OFFSET(0x6b, 0) };

		const auto instructionBytes = resume.address() - target.address();
		for (std::size_t i = 0; i < instructionBytes; i++) {
			REL::safe_write(target.address() + i, REL::NOP);
		}

		Patch p{ resume.address() };
		p.ready();

		SKSE::AllocTrampoline(128);
		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(
			target.address(),
			trampoline.allocate(p));
		logger::info("\t\tInstalled VR tfc fix; overwrote {} bytes at {:x}"sv, instructionBytes, target.address());
	}
}

void Hooks::Install()
{
	PhotoMode::InstallHooks();
	Screenshot::InstallHooks();
	LoadScreen::InstallHooks();

	if (stl::IsVR()) {
		VRTFCFix::InstallHooks();
	}
}
