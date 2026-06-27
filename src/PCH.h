#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#define IMGUI_DEFINE_MATH_OPERATORS
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING

#define MANAGER(T) T::Manager::GetSingleton()

#include "RE/Skyrim.h"
#include "REX/REX/Singleton.h"
#include "SKSE/SKSE.h"

#include <codecvt>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <DirectXTex.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/unordered_node_map.hpp>
#include <freetype/freetype.h>
#include <glaze/glaze.hpp>
#include <rapidfuzz/rapidfuzz_all.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <srell.hpp>
#include <xbyak/xbyak.h>

#include "ImGui/Backend/imgui_impl_win32.h"
#include "imgui_internal.h"
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <ClibUtil/RNG.hpp>
#include <ClibUtil/editorID.hpp>
#include <ClibUtil/hash.hpp>
#include <ClibUtil/simpleINI.hpp>
#include <ClibUtil/string.hpp>

#define DLLEXPORT __declspec(dllexport)

using namespace std::literals;
using namespace clib_util;
using namespace string::literals;

namespace logger = SKSE::log;

using EventResult = RE::BSEventNotifyControl;

using KEY = RE::BSWin32KeyboardDevice::Key;
using GAMEPAD_DIRECTX = RE::BSWin32GamepadDevice::Key;
using GAMEPAD_ORBIS = RE::BSPCOrbisGamepadDevice::Key;
using MOUSE = RE::BSWin32MouseDevice::Key;

template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

template <class K, class D, class H = boost::hash<K>, class KEqual = std::equal_to<K>>
using NodeMap = boost::unordered_node_map<K, D, H, KEqual>;

template <class K, class D, class H = boost::hash<K>, class KEqual = std::equal_to<K>>
using FlatMap = boost::unordered_flat_map<K, D, H, KEqual>;

template <class K, class H = boost::hash<K>, class KEqual = std::equal_to<K>>
using FlatSet = boost::unordered_flat_set<K, H, KEqual>;

struct string_hash
{
	using is_transparent = void;

	std::size_t operator()(const char* str) const
	{
		return boost::hash<std::string_view>{}(str);
	}

	std::size_t operator()(std::string_view str) const
	{
		return boost::hash<std::string_view>{}(str);
	}

	std::size_t operator()(const std::string& str) const
	{
		return boost::hash<std::string>{}(str);
	}
};

template <class D>
using StringMap = FlatMap<std::string, D, string_hash, std::equal_to<>>;

using StringSet = FlatSet<std::string, string_hash, std::equal_to<>>;

namespace stl
{
	using namespace SKSE::stl;

	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[0] };
		T::func = vtbl.write_vfunc(T::idx, T::thunk);
	}

	template <class T, std::size_t BYTES>
	void hook_function_prologue(std::uintptr_t a_src)
	{
		struct Patch : Xbyak::CodeGenerator
		{
			Patch(std::uintptr_t a_originalFuncAddr, std::size_t a_originalByteLength)
			{
				// Hook returns here. Execute the restored bytes and jump back to the original function.
				for (size_t i = 0; i < a_originalByteLength; ++i) {
					db(*reinterpret_cast<std::uint8_t*>(a_originalFuncAddr + i));
				}

				jmp(ptr[rip]);
				dq(a_originalFuncAddr + a_originalByteLength);
			}
		};

		Patch p(a_src, BYTES);
		p.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(a_src, T::thunk);

		auto alloc = trampoline.allocate(p.getSize());
		std::memcpy(alloc, p.getCode(), p.getSize());

		T::func = reinterpret_cast<std::uintptr_t>(alloc);
	}

	inline bool IsVR()
	{
		return REL::Module::IsVR();
	}

	// ImageSpaceManager runtime data: NG splits it into SE/AE vs VR structs and its non-VR
	// GetRuntimeData() carries no VR offset, so each access must pick the runtime-correct one.
	[[nodiscard]] inline RE::ImageSpaceBaseData*& imagespace_current(RE::ImageSpaceManager* a_mgr)
	{
		return REL::Module::IsVR() ? a_mgr->GetVRRuntimeData().currentBaseData : a_mgr->GetRuntimeData().currentBaseData;
	}
	[[nodiscard]] inline RE::ImageSpaceBaseData*& imagespace_override(RE::ImageSpaceManager* a_mgr)
	{
		return REL::Module::IsVR() ? a_mgr->GetVRRuntimeData().overrideBaseData : a_mgr->GetRuntimeData().overrideBaseData;
	}
}

// Single cross-runtime CommonLibSSE-NG DLL: offsets and engine fields resolve at load time.
// OFFSET_3 supplies a distinct VR byte offset; OFFSET defaults VR to the SE value.
#define OFFSET(se, ae) REL::VariantOffset(se, ae, se)
#define OFFSET_3(se, ae, vr) REL::VariantOffset(se, ae, vr)

// Engine fields that NG keeps in RUNTIME_DATA blocks are reached through its
// runtime-detecting accessors. These aliases name the accessor for each struct.
#define RENDERER_DATA(a_obj)    (a_obj)->GetRuntimeData()
#define MAIN_DATA(a_obj)        (a_obj)->GetRuntimeData()
#define ACTOR_DATA(a_obj)       (a_obj)->GetActorRuntimeData()
#define CAMERA_DATA(a_obj)      (a_obj)->GetRuntimeData2()    // worldFOV etc.
#define MISTMENU_DATA(a_obj)    (a_obj)->GetRuntimeData()
#define PLAYER_GAMESTATE(a_obj) (a_obj)->GetGameStatsData()   // byCharGenFlag etc.
#define CONTROLMAP_DATA(a_obj)  (a_obj)->GetRuntimeData()
#define JOURNALMENU_DATA(a_obj) (a_obj)->GetRuntimeData()

#include "Cache.h"
#include "Translation.h"
#include "Version.h"
