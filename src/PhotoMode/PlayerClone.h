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
		// Per-bone local (parent-relative) transforms, keyed by node name. Used to snapshot a skeleton
		// once at a known-good moment (see CapturePose) and reapply it later without re-querying the
		// live actor, which in VR can already reflect camera-fly-induced drift by the time it matters.
		using BonePose = std::unordered_map<std::string, RE::NiTransform>;

		// Snapshots every named bone's current local transform. Call this on the player before anything
		// (the VR free camera's play-space-flying hack in particular) has a chance to move things, and
		// feed the result into Spawn() -- see its parameter comment for why a live query later isn't
		// reliable in VR.
		[[nodiscard]] static BonePose CapturePose(RE::Actor* a_actor);

		// a_originalPos/a_originalAngleZ/a_originalPose: the player's position/facing/skeleton captured
		// before anything (the VR free camera's play-space-flying hack in particular) had a chance to
		// move it -- see the call site for why a live query at spawn time isn't reliable in VR.
		void Spawn(const RE::NiPoint3& a_originalPos, float a_originalAngleZ, const BonePose& a_originalPose);
		void Despawn();
		// Per-spawn setup once the clone's 3D has streamed in (call each frame while active; it runs
		// once): re-arm facial animation, replay the player's active-effect visuals and readied-spell
		// charge art, then mirror the captured pose onto the clone's skeleton.
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
			spawnPose.clear();
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
		RE::NiPoint3        spawnPos{};      // the player's exact position at spawn; the clone is pinned here
		BonePose            spawnPose{};     // the player's captured skeleton; mirrored onto the clone once posed
		bool                poseApplied{ false };
		// One-shot: re-run DoReset3D once the head 3D has streamed in, to arm facial animation that
		// the Spawn-time reset (head not yet loaded) couldn't.
		bool faceReset{ false };
	};
}
