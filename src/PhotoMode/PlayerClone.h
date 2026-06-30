#pragma once

namespace PhotoMode
{
	// A photographable stand-in for the player in VR photo mode. The real first-person body
	// rides the free camera, so a static copy is placed where the player was standing and the
	// camera flies around it. Built on the appearance-copy technique from PlayerMannequin
	// (Adrien Melia / Sylennus, MIT) — see CREDITS.md: a runtime NPC base that borrows the
	// player's facegen (faceNPC) plus race/sex/weight/tint, wearing the player's gear.
	class PlayerClone
	{
	public:
		void Spawn();
		void Despawn();
		// Per-spawn setup once the clone's 3D has streamed in (call each frame while active; it runs
		// once): re-arm facial animation, then replay the player's active-effect visuals and readied-
		// spell charge art. The clone poses itself via idle animation (AI enabled), so no bone-copy.
		void               ApplyPose();
		[[nodiscard]] bool IsSpawned() const { return static_cast<bool>(cloneRef); }
		// The spawned clone actor, or nullptr. Lets the Character tab target the photographed clone
		// instead of the (hidden) player in VR.
		[[nodiscard]] RE::Actor* GetClone() const
		{
			const auto ref = cloneRef.get();
			return ref ? ref->As<RE::Actor>() : nullptr;
		}
		// True once the clone's 3D has streamed in and its pose/effects have been applied — used
		// to defer the VR time-freeze until the clone exists (a freeze would stall its 3D load).
		[[nodiscard]] bool IsReady() const { return poseApplied; }
		// Drop the cached runtime base and ref without touching them. A save load tears down the
		// form DB, leaving cloneBase dangling; call this then so the next Spawn recreates cleanly.
		void Invalidate()
		{
			cloneBase = nullptr;
			cloneRef = {};
			poseApplied = false;
			faceReset = false;
		}

	private:
		void        UpdateAppearance(RE::TESNPC* a_clone, RE::TESNPC* a_playerBase) const;
		static void CopyWornEquipment(RE::Actor* a_target, RE::Actor* a_source);
		// Replay the player's active-effect visuals (shaders/art) on the clone, so auras like
		// fire cloaks, shouts or potion glows show in the shot. Needs the clone's 3D loaded.
		static void CopyVisualEffects(RE::Actor* a_target, RE::Actor* a_source);
		// Re-create the readied-spell charge art (e.g. flames in hand) on the clone's hand magic
		// nodes. Separate from active effects. Needs the clone's 3D loaded.
		static void CopyHandMagic(RE::Actor* a_target, RE::Actor* a_source);

		RE::TESNPC*         cloneBase{ nullptr };  // runtime base, reused across sessions
		RE::ObjectRefHandle cloneRef{};
		RE::NiPoint3        spawnPos{};  // the player's exact position at spawn; the clone is pinned here
		bool                poseApplied{ false };
		// One-shot: re-run DoReset3D once the head 3D has streamed in, to arm facial animation that
		// the Spawn-time reset (head not yet loaded) couldn't.
		bool faceReset{ false };
	};
}
