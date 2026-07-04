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
		// Main::Render writes the screenshot via DebugNotification(path, kPNG) reading the kSCREENSHOT
		// target; intercept that call to apply PhotoMode's capture (overlay / painting / folder). The
		// VR call site is 0x496 (RE'd from SkyrimVR 1.4.15); the SE/AE-only offset landed on the wrong
		// instruction and crashed when a shot was taken in VR.
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(35556, 36555), OFFSET_3(0x48E, 0x454, 0x496) };  // Main::Render
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
				// SE/AE have the model loaded by now, so texture it inline. VR streams it in asynchronously
				// after this call returns; MistMenuAdvanceMovie applies it once it arrives (see below).
				MANAGER(LoadScreen)->NotifyModelReady(MISTMENU_DATA(a_this).loadScreenModel.get());
			} else {
				func(a_this, a_scale, a_rotateOffset, a_translateOffset, a_cameraShotPath);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// VR-only: the load-screen model NIF streams in asynchronously, so it is not present when the
	// InitLoadScreen3D setup call above runs. MistMenu::AdvanceMovie re-runs setup once the model lands;
	// we piggyback there to texture the canvas. In VR the +0xE0 slot (CommonLib's cameraPathNode) holds a
	// BSResource entry, not the node directly -- its name field is the model path, so walking it as a node
	// crashes. Detect: if the slot's first qword is a vtable in the game module it is the NiNode itself;
	// otherwise it is the entry and the NiPointer<NiNode> lives at entry+0x28 (BSResource::Entry::data).
	RE::NiNode* ResolveLoadScreenModel(RE::MistMenu* a_this)
	{
		const auto slot = reinterpret_cast<std::uintptr_t>(MISTMENU_DATA(a_this).cameraPathNode.get());
		if (!slot) {
			return nullptr;
		}
		const auto base = REL::Module::get().base();
		const auto first = *reinterpret_cast<std::uintptr_t*>(slot);
		if (first >= base && first < base + 0x4000000) {  // looks like a NiObject vtable -> node itself
			return reinterpret_cast<RE::NiNode*>(slot);
		}
		return *reinterpret_cast<RE::NiNode**>(slot + 0x28);  // BSResource::Entry::data (NiPointer<NiNode>)
	}

	struct MistMenuAdvanceMovie
	{
		static void thunk(RE::MistMenu* a_this, float a_interval, std::uint32_t a_currentTime)
		{
			func(a_this, a_interval, a_currentTime);
			// Only touch the model once the engine finished its deferred load-screen setup on it (the
			// cameraPathNode tree is fully built); before that we'd race the async loader and walk a
			// half-built node. loadScreenModelReady is the engine's "load-screen setup done" flag.
			if (const auto vrData = a_this->GetVRRuntimeData(); vrData && vrData->loadScreenModelReady) {
				MANAGER(LoadScreen)->NotifyModelReady(ResolveLoadScreenModel(a_this));
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline constexpr std::size_t            idx{ 0x5 };  // IMenu::AdvanceMovie
	};

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(51048, 51929), OFFSET_3(0x384, 0x27B, 0x394) };
		stl::write_thunk_call<GetLoadScreenModel>(target.address());

		REL::Relocation<std::uintptr_t> target2{ RELOCATION_ID(51454, 52313), OFFSET_3(0x1D1, 0x1C3, 0x210) };
		stl::write_thunk_call<InitLoadScreen3D>(target2.address());

		if (REL::Module::IsVR()) {
			stl::write_vfunc<RE::MistMenu, MistMenuAdvanceMovie>();
		}
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
	// Ghidra-confirmed (adversarial review flagged this, first version used RDI as scratch instead of
	// RCX): the function stashes its enable/disable bool parameter in DIL at +0x0d, well before this
	// patched region, and doesn't re-check it (TEST DIL,DIL) until +0x92 -- after our resume point at
	// +0x6b. RDI is genuinely safe from a callee-saved standpoint (the function's own PUSH/POP RDI
	// prologue/epilogue restores the caller's value regardless of what we do to it in between), but
	// clobbering it here destroyed that still-live parameter, making the +0x92 branch effectively read
	// the FreeCameraState pointer's low byte instead of the real toggle state. Using RCX instead avoids
	// this entirely -- it's already needed in RCX for the call at +0x6b, so nothing else needs touching.
	struct Patch : Xbyak::CodeGenerator
	{
		explicit Patch(std::uintptr_t a_rtn)
		{
			Xbyak::Label retLab;

			mov(rcx, qword[rbx + 0xd8]);     // rcx = FreeCameraState (also what the call at +0x6b needs)
			movss(dword[rcx + 0x30], xmm0);  // Position.x = xmm0 (already [rsp+0x20])
			movss(xmm1, dword[rsp + 0x24]);
			movss(dword[rcx + 0x34], xmm1);  // Position.y = [rsp+0x24]
			movss(xmm0, dword[rsp + 0x28]);
			movss(dword[rcx + 0x38], xmm0);  // Position.z = [rsp+0x28]

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

		// The shared trampoline is already allocated once in main.cpp's SKSEPlugin_Load -- a second
		// AllocTrampoline call here would free that buffer (SKSE::Trampoline::set_trampoline calls
		// release() first), corrupting the stubs PhotoMode/Screenshot/LoadScreen's InstallHooks already
		// wrote into it (all three run before this, per Hooks::Install's call order), since their JMPs
		// would then point into freed memory.
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
