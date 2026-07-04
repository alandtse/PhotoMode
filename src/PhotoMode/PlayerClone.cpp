// The player-cloning technique here (a runtime NPC base that borrows the player's facegen
// via faceNPC, plus the SetActorBaseDataFlag relocation) is adapted from PlayerMannequin
// by Adrien Melia (Sylennus), MIT-licensed: https://github.com/Sylennus/PlayerMannequin
// See CREDITS.md for the full notice.

#include "PlayerClone.h"

namespace PhotoMode
{
	namespace
	{
		void SetActorBaseDataFlag(RE::TESActorBaseData* a_data, RE::ACTOR_BASE_DATA::Flag a_flag, bool a_enable)
		{
			if (!a_data) {
				return;
			}
			using func_t = decltype(&SetActorBaseDataFlag);
			static REL::Relocation<func_t> func{ REL::RelocationID(14261, 14383) };
			func(a_data, a_flag, a_enable);
		}

		template <class F>
		void VisitNodes(RE::NiAVObject* a_obj, F&& a_fn)
		{
			if (!a_obj) {
				return;
			}
			a_fn(a_obj);
			if (const auto node = a_obj->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					VisitNodes(child.get(), a_fn);
				}
			}
		}

		// The engine refers to the hand magic node as "NPC[L/R]MagicNode[.Mag]", but skeleton.nif
		// usually names the actual node "NPC L MagicNode" (spaces, no tag), so GetObjectByName with
		// the engine string misses. Match by substring, falling back to the hand bone.
		RE::NiAVObject* FindHandMagicNode(RE::NiAVObject* a_root, bool a_left)
		{
			RE::NiAVObject* magicNode = nullptr;
			RE::NiAVObject* handNode = nullptr;
			VisitNodes(a_root, [&](RE::NiAVObject* a_node) {
				const auto* raw = a_node->name.c_str();
				if (!raw) {
					return;
				}
				const std::string_view name{ raw };
				const bool             side = a_left ? (name.find("L Magic") != std::string_view::npos || name.find("LMag") != std::string_view::npos) :
				                                       (name.find("R Magic") != std::string_view::npos || name.find("RMag") != std::string_view::npos);
				if (!magicNode && name.find("Magic") != std::string_view::npos && side) {
					magicNode = a_node;
				}
				const bool handSide = a_left ? (name.find("L Hand") != std::string_view::npos || name.find("LHnd") != std::string_view::npos) :
				                               (name.find("R Hand") != std::string_view::npos || name.find("RHnd") != std::string_view::npos);
				if (!handNode && name.find("Hand") != std::string_view::npos && handSide) {
					handNode = a_node;
				}
			});
			return magicNode ? magicNode : handNode;
		}
	}

	void PlayerClone::UpdateAppearance(RE::TESNPC* a_clone, RE::TESNPC* a_playerBase) const
	{
		a_clone->race = a_playerBase->race;
		a_clone->faceNPC = a_playerBase;  // borrow the player's facegen head
		a_clone->height = a_playerBase->height;
		a_clone->weight = a_playerBase->weight;
		a_clone->bodyTintColor = a_playerBase->bodyTintColor;
		SetActorBaseDataFlag(a_clone, RE::ACTOR_BASE_DATA::Flag::kFemale, a_playerBase->GetSex() == RE::SEXES::kFemale);

		// A fresh base leaves the attack-data map null, which the engine's attack eval dereferences and
		// CTDs when the AI clone is interacted with. Borrow the player's (same race, so it's valid);
		// NiPointer assignment keeps the refcount balanced.
		static_cast<RE::BGSAttackDataForm*>(a_clone)->attackDataMap =
			static_cast<RE::BGSAttackDataForm*>(a_playerBase)->attackDataMap;

		// combatStyle is null on a fresh base, and the background combat-evaluation job (queued on
		// interaction/activation) dereferences it and CTDs. It's part of the CK's "Traits" template
		// category (bundled with race/height/weight/voice type), which we deliberately don't borrow from
		// the Mannequin -- CopyFromTemplateForms would overwrite our own reskin with the Mannequin's. Copy
		// just this one field from the Mannequin instead of the player, since its combat style is the
		// inert one actually meant for a display prop; StopCombat() at spawn keeps real combat out either way.
		if (const auto mannequinBase = RE::TESForm::LookupByID<RE::TESNPC>(0x89A85)) {
			a_clone->SetCombatStyle(mannequinBase->GetCombatStyle());
		}

		// Match the player's hair so helmets/face render correctly.
		for (std::int8_t i = 0; i < a_playerBase->numHeadParts; ++i) {
			if (const auto headPart = a_playerBase->headParts ? a_playerBase->headParts[i] : nullptr;
				headPart && headPart->type == RE::BGSHeadPart::HeadPartType::kHair) {
				a_clone->ChangeHeadPart(headPart);
			}
		}
	}

	void PlayerClone::CopyWornEquipment(RE::Actor* a_target, RE::Actor* a_source)
	{
		const auto equipManager = RE::ActorEquipManager::GetSingleton();
		if (!equipManager) {
			return;
		}

		// EquipObject only equips items the actor already owns, so give the clone its own copy
		// of each piece first, then equip it.
		const auto giveAndEquip = [&](RE::TESBoundObject* a_object) {
			if (!a_object) {
				return;
			}
			a_target->AddObjectToContainer(a_object, nullptr, 1, nullptr);
			equipManager->EquipObject(a_target, a_object);
		};

		using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
		for (const auto slot : { Slot::kHead, Slot::kBody, Slot::kHands, Slot::kForearms, Slot::kAmulet,
				 Slot::kRing, Slot::kFeet, Slot::kCalves, Slot::kShield, Slot::kCirclet, Slot::kHair }) {
			giveAndEquip(a_source->GetWornArmor(slot, true));
		}

		// Hands hold weapons rather than armor.
		const auto equipHand = [&](bool a_leftHand) {
			if (const auto worn = a_source->GetEquippedObject(a_leftHand)) {
				giveAndEquip(worn->As<RE::TESBoundObject>());
			}
		};
		equipHand(false);
		equipHand(true);
	}

	void PlayerClone::CopyVisualEffects(RE::Actor* a_target, RE::Actor* a_source)
	{
		const auto magicTarget = a_source->AsMagicTarget();
		const auto effects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
		if (!effects) {
			return;
		}

		std::size_t shaders = 0, arts = 0;
		for (const auto& activeEffect : *effects) {
			const auto base = activeEffect ? activeEffect->GetBaseObject() : nullptr;
			if (!base) {
				continue;
			}
			const auto& data = base->data;
			// -1 duration = persist until despawn; these reattach to the clone's 3D.
			if (data.effectShader) {
				a_target->ApplyEffectShader(data.effectShader);
				++shaders;
			}
			if (data.hitEffectArt) {
				a_target->ApplyArtObject(data.hitEffectArt);
				++arts;
			}
		}
		logger::info("PlayerClone: active-effect visuals applied (shaders={} arts={})"sv, shaders, arts);
	}

	void PlayerClone::CopyHandMagic(RE::Actor* a_target, RE::Actor* a_source)
	{
		const auto clone3D = a_target->Get3D(false);
		if (!clone3D) {
			return;
		}

		const auto& runtime = a_source->GetActorRuntimeData();
		const auto  applyHand = [&](std::uint32_t a_slot, bool a_left) {
            const auto spell = runtime.selectedSpells[a_slot];
            const auto avEffect = spell ? spell->GetAVEffect() : nullptr;
            const auto art = avEffect ? avEffect->data.castingArt : nullptr;
            if (!art) {
                return;
            }
            // Attach the charge art to the clone's matching hand magic node so it sits in the
            // palm like the live readied-spell visual.
            const auto node = FindHandMagicNode(clone3D, a_left);
            a_target->ApplyArtObject(art, -1.0f, nullptr, false, false, node);
            logger::info("PlayerClone: applied hand magic '{}' art on {} node ({})"sv,
				 spell->GetName(), a_left ? "left" : "right", node ? node->name.c_str() : "none");
		};

		applyHand(RE::Actor::SlotTypes::kLeftHand, true);
		applyHand(RE::Actor::SlotTypes::kRightHand, false);
	}

	PlayerClone::BonePose PlayerClone::CapturePose(RE::Actor* a_actor)
	{
		BonePose pose;
		if (const auto root = a_actor ? a_actor->Get3D(false) : nullptr) {
			VisitNodes(root, [&](RE::NiAVObject* a_node) {
				if (const auto* name = a_node->name.c_str(); name && *name) {
					pose.insert_or_assign(name, a_node->local);
				}
			});
		}
		return pose;
	}

	void PlayerClone::ApplyBonePose(RE::Actor* a_actor, const BonePose& a_pose)
	{
		const auto root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root || a_pose.empty()) {
			return;
		}
		VisitNodes(root, [&](RE::NiAVObject* a_node) {
			const auto* name = a_node->name.c_str();
			if (!name || !*name) {
				return;
			}
			const auto it = a_pose.find(name);
			if (it == a_pose.end()) {
				return;
			}
			const std::string_view nameView{ name };
			if (nameView.find("Root") != std::string_view::npos || nameView.find("COM") != std::string_view::npos) {
				a_node->local.rotate = it->second.rotate;
				a_node->local.scale = it->second.scale;
			} else {
				a_node->local = it->second;
			}
		});
		RE::NiUpdateData updateData{};
		root->Update(updateData);
	}

	void PlayerClone::Spawn(const RE::NiPoint3& a_originalPos, float a_originalAngleZ, const BonePose& a_originalPose)
	{
		if (IsSpawned()) {
			return;
		}
		poseApplied = false;
		faceReset = false;
		positionPinned = false;
		settleWaitFrames = 0;
		spawnPose = a_originalPose;

		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto playerBase = player ? player->GetActorBase() : nullptr;
		if (!playerBase) {
			return;
		}
		// Pin the clone at the caller-supplied position, not a live player->GetPosition() query: VR's
		// free-camera fly hack (Manager's DriveVRCamera) offsets PlayerWorldNode to simulate movement,
		// and the engine's own room-scale tracking -- which normally turns real physical HMD movement
		// into actual player position updates -- can't tell that offset apart from a genuine physical
		// step, updating data.location to match. By the time Spawn() runs, GetPosition() may already
		// reflect wherever the camera flew to rather than where the player was actually standing.
		spawnPos = a_originalPos;  // pin the clone here; PlaceObjectAtMe shoves it out of the capsule

		if (!cloneBase) {
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESNPC>();
			cloneBase = factory ? factory->Create() : nullptr;
			if (!cloneBase) {
				logger::error("PlayerClone: failed to create clone base"sv);
				return;
			}

			// Borrow the Mannequin's AI brain via the template system (not a copy): a fresh base's null
			// combat/idle data crashes the AI update, and copying would share heap-owned members like
			// faceData so expressions would corrupt the shared base. The template gives a real high
			// process — the source of facegen + idle playback — while appearance stays ours.
			using TUF = RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG;
			if (const auto mannequinBase = RE::TESForm::LookupByID<RE::TESNPC>(0x89A85)) {
				cloneBase->baseTemplateForm = mannequinBase;
				cloneBase->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUsesTemplate);
				// Borrow only the AI brain. NOT kAttackData: the Mannequin has no attack-data map, and
				// CopyFromTemplateForms would overwrite the valid one we copy in UpdateAppearance with
				// the Mannequin's null. kSpells is included because a fresh base's null/uninitialized
				// spell list crashes the background combat job's spell-gathering pass once time is
				// unfrozen and the AI actually starts ticking (RE'd from a CTD in Actor::UpdateCombat).
				cloneBase->actorData.templateUseFlags.set(TUF::kAIData, TUF::kAIPackages, TUF::kAIDefPackList, TUF::kSpells);
				// Borrowing a real AI brain also gives the clone ambient/idle Hello barks; drop the voice
				// type so it has nothing to say (idle animation and expressions are unaffected).
				cloneBase->SetObjectVoiceType(nullptr);

				// The kAIData template flag doesn't actually resolve every field through the getters --
				// logged confidence=Average on a fresh base vs. the Mannequin's own Cowardly, meaning a
				// standing NPC that fights back if provoked instead of one that never engages. aiData is a
				// plain bitfield struct (no owned pointers), so copy it wholesale rather than borrowing.
				cloneBase->aiData = mannequinBase->aiData;
			} else {
				logger::warn("PlayerClone: vanilla Mannequin base (0x89A85) not found; clone may be inert"sv);
			}
		}
		UpdateAppearance(cloneBase, playerBase);

		// The clone spawns exactly where the player is standing, so PlaceObjectAtMe's capsule overlaps
		// the player's -- kNoSim (below, in ApplyPose) only suspends the CLONE's own collision briefly,
		// but the PLAYER's side of that same overlap is untouched, so once kNoSim clears the two
		// capsules can push each other apart on the very next physics tick, after our position pin has
		// already run. Drop the player's collision too (tcl's actual mechanism) for the same window, so
		// there's nothing on either side to resolve; ApplyPose restores it once the pin lands.
		player->SetCollision(false);

		const auto ref = player->PlaceObjectAtMe(cloneBase, true);
		if (!ref) {
			logger::error("PlayerClone: PlaceObjectAtMe failed"sv);
			player->SetCollision(true);  // don't leave the player permanently collision-free on failure
			return;
		}
		cloneRef = ref->CreateRefHandle();

		ref->data.angle = { 0.0f, 0.0f, a_originalAngleZ };
		if (const auto cloneActor = ref->As<RE::Actor>()) {
			// AI stays on through spawn -- ApplyPose() disables it once the one-shot facegen/expression
			// setup finishes. AI is what drives expression morphing and idle animation (confirmed: with it
			// off from the start, the clone goes fully stiff), but it's also what makes the clone walk
			// away and talk on its own once time passes (the long-documented "mannequins come alive" bug --
			// real, unpatched mannequins in this game version do the exact same thing). Letting it run just
			// long enough to bake in a good pose/expression, then freezing it, is the least bad tradeoff.
			cloneActor->StopCombat();
			cloneActor->AllowPCDialogue(false);  // let the player still grab/pose it; just no "Talk" prompt
			CopyWornEquipment(cloneActor, player);
			cloneActor->DoReset3D(true);  // rebuild the biped so the equipped gear renders
		}
		logger::info("PlayerClone: spawned photo clone"sv);
	}

	void PlayerClone::ApplyPose()
	{
		const auto cloneRefPtr = cloneRef.get();
		const auto cloneActor = cloneRefPtr ? cloneRefPtr->As<RE::Actor>() : nullptr;
		if (!cloneActor) {
			return;
		}

		const auto player = RE::PlayerCharacter::GetSingleton();

		// The borrowed AI brain lets the running package interrupt for Hellos/idle chatter/reactions to
		// the player being nearby (RE::TESPackage::InterruptFlag) -- the voice-type/AllowPCDialogue calls
		// in Spawn() don't cover this (those only stop player-initiated dialogue, not the AI's own
		// attempts), and the AI periodically re-evaluates/refreshes its package, undoing a one-time
		// clear. Clearing the per-instance override (not the shared package form) every frame keeps it
		// silenced regardless.
		if (const auto proc = cloneActor->GetActorRuntimeData().currentProcess) {
			proc->currentPackage.modifiedInterruptFlag = 0;
		}

		// StopCombat() at spawn is a one-shot -- if anything provokes combat afterward (a bump, a HIGGS
		// grab registering as an impact, a hostile actor drawing it in), nothing stops it from actually
		// entering and staying in combat. Same class of bug as the interrupt flag above: re-assert it
		// every frame so a display prop can never actually fight back.
		if (cloneActor->IsInCombat()) {
			cloneActor->StopCombat();
		}

		if (poseApplied) {
			return;
		}

		if (!player) {
			return;
		}

		// Wait for the clone's 3D (head + body) to stream in — it loads a few frames after PlaceObjectAtMe.
		const auto cloneRoot = cloneActor->Get3D(false);
		if (!cloneRoot) {
			return;  // clone 3D not loaded yet — try again next frame
		}

		// Arm facial animation. The engine only switches a face into its per-frame-morphed state during
		// DoReset3D's model update, and only when GetFaceGenAnimationData() is non-null at that instant.
		// The Spawn-time DoReset3D runs before the head 3D has streamed in, so it's skipped (RE'd in
		// AIProcess::Update3DModel_Impl). Re-run it once now that the head exists, then settle a frame
		// before posing so the rebuilt model is stable.
		if (!faceReset) {
			cloneActor->DoReset3D(true);
			faceReset = true;
			return;
		}

		// Complete the facegen process link that the engine's gated path skips for a runtime clone: the
		// per-frame face morph reads middleHigh->faceAnimationData (which DoReset3D leaves null here), so
		// point it at the head-node data, or expressions/phonemes never tick.
		if (const auto proc = cloneActor->GetActorRuntimeData().currentProcess) {
			if (const auto mid = proc->middleHigh; mid && !mid->faceAnimationData) {
				mid->faceAnimationData = cloneActor->GetFaceGenAnimationData();
			}
		}

		// Pin X/Y exactly, but land a small clearance above the captured Z rather than exactly at it:
		// the captured spot can be an unusual stance (standing on a raised, non-flat object like a
		// brazier), and forcing the clone's capsule to that exact Z can embed it slightly into that
		// object's collision -- reading later as a teleport once something (AI re-enabling for Poses/
		// Expressions, or just the controller's own ground-support check) resolves the embed.
		constexpr float kSpawnDropClearance = 60.0f;  // ~2 feet; game units, not meters
		const auto      charController = cloneActor->GetCharController();
		if (!positionPinned) {
			if (charController) {
				charController->flags.set(RE::CHARACTER_FLAGS::kNoSim);
			}
			cloneActor->SetPosition(spawnPos + RE::NiPoint3{ 0.0f, 0.0f, kSpawnDropClearance }, true);
			if (charController) {
				charController->flags.reset(RE::CHARACTER_FLAGS::kNoSim);
				// kSupport doesn't get cleared by SetPosition -- it's confirmed (via logging) to read
				// stale-true on the very next frame, carrying over ground-contact state from before this
				// teleport rather than reflecting the new (now airborne) position. Left alone, the wait
				// below exits immediately, capturing the settle anchor 60 units too high. Force it false so
				// the controller has to freshly re-earn it once it actually lands.
				charController->flags.reset(RE::CHARACTER_FLAGS::kSupport);
			}
			positionPinned = true;
			return;  // give the drop at least one real frame before checking on it
		}

		// Releasing kNoSim and finishing setup in the same call, with no frame in between, leaves no
		// time for gravity to actually run: the clone would still be airborne when EnableAI(false) below
		// locks things in, then visibly fall over the following frames with AI already disabled -- a
		// falling humanoid with no dedicated falling animation can blend into a walk/run cycle, reading
		// as "the clone started walking" rather than "the clone hasn't landed yet". Wait for the
		// controller to actually report grounded (kSupport) before finishing, capped so a spot with
		// nothing to land on (still theoretically possible) can't stall setup forever.
		constexpr int kMaxSettleWaitFrames = 30;  // half a second at 60fps
		const bool    supported = charController && charController->flags.any(RE::CHARACTER_FLAGS::kSupport);
		logger::info("PlayerClone: settle-wait frame={} z={:.1f} supported={}"sv, settleWaitFrames, cloneActor->GetPosition().z, supported);
		if (!supported && settleWaitFrames < kMaxSettleWaitFrames) {
			++settleWaitFrames;
			return;
		}
		logger::info("PlayerClone: settle-wait done at frame={} z={:.1f} (spawnPos.z={:.1f})"sv, settleWaitFrames, cloneActor->GetPosition().z, spawnPos.z);

		// Restore the player's collision (dropped in Spawn() for this same overlapping-capsule window)
		// now that the pin has landed -- both sides are clear of each other from here on.
		player->SetCollision(true);

		// Mirror the player's captured pose (spawnPose, taken at activation -- see Spawn()'s parameter
		// comment for why a live query here isn't reliable in VR) onto the clone's skeleton. Run this
		// last, after AI has settled the clone into its own idle pose, or the animation graph's next
		// tick (while AI is still briefly on) would immediately overwrite it.
		ApplyBonePose(cloneActor, spawnPose);

		// Now that the clone's 3D exists, replay the player's active-effect visuals and the
		// readied-spell charge art onto it.
		CopyVisualEffects(cloneActor, player);
		CopyHandMagic(cloneActor, player);

		// The one-shot setup above needed AI on to drive expression morphing and idle animation into a
		// good resting pose. Freeze it here: left running, the AI eventually walks the clone away and
		// talks to the player on its own (the same long-standing "mannequins come alive" engine bug real,
		// unpatched mannequins in this game version also exhibit) -- StopCombat/interrupt-flag suppression
		// above couldn't fully contain it because it isn't one specific reaction, it's the AI running at all.
		cloneActor->EnableAI(false);

		// Capture where the clone actually settled (not spawnPos -- the drop-and-settle above can land
		// it on a slightly different Z/orientation than what was captured) as the anchor ReseatIfDrifted
		// checks against every frame from here on.
		anchorPos = cloneActor->GetPosition();
		anchorAngleZ = cloneActor->GetAngleZ();
		anchorSet = true;
		logger::info("PlayerClone: anchor captured pos=({:.1f},{:.1f},{:.1f}) angleZdeg={:.1f}"sv,
			anchorPos.x, anchorPos.y, anchorPos.z, RE::rad_to_deg(anchorAngleZ));

		poseApplied = true;
	}

	void PlayerClone::ReseatIfDrifted()
	{
		if (!anchorSet) {
			return;
		}
		const auto cloneRefPtr = cloneRef.get();
		const auto cloneActor = cloneRefPtr ? cloneRefPtr->As<RE::Actor>() : nullptr;
		if (!cloneActor) {
			return;
		}

		// Re-enabling AI for Poses/Expressions can still nudge the clone slightly (its own idle root
		// motion, or the character controller reacting to something) without necessarily "teleporting"
		// far enough for the do-nothing-package/settle-wait fixes' symptoms to apply. Position tolerance
		// is generous enough to not fight normal idle sway; angle tolerance likewise for subtle turning
		// idles, but anything past either is drift, not intentional animation.
		constexpr float kPositionDriftTolerance = 10.0f;  // game units
		constexpr float kAngleDriftToleranceDeg = 10.0f;

		const auto  currentPos = cloneActor->GetPosition();
		const float posDrift = (currentPos - anchorPos).Length();

		// GetAngleZ() is radians; wrap the difference to [-pi, pi] before converting to degrees so e.g.
		// 359 vs 1 degree reads as 2 degrees apart, not 358.
		float angleDriftRad = std::fmod(cloneActor->GetAngleZ() - anchorAngleZ, 2.0f * std::numbers::pi_v<float>);
		if (angleDriftRad > std::numbers::pi_v<float>) {
			angleDriftRad -= 2.0f * std::numbers::pi_v<float>;
		} else if (angleDriftRad < -std::numbers::pi_v<float>) {
			angleDriftRad += 2.0f * std::numbers::pi_v<float>;
		}
		const float angleDrift = std::abs(RE::rad_to_deg(angleDriftRad));

		// DEBUG: log on every AI-enabled edge, and periodically while AI is enabled, to compare where we
		// think the clone is against the anchor both during animation and without.
		static bool previousAIEnabled = false;
		static int  logThrottle = 0;
		const bool  aiEnabled = cloneActor->IsAIEnabled();
		if (aiEnabled != previousAIEnabled || (aiEnabled && ++logThrottle >= 15)) {
			logThrottle = 0;
			logger::info("PlayerClone: pos=({:.1f},{:.1f},{:.1f}) anchor=({:.1f},{:.1f},{:.1f}) posDrift={:.1f} angleDrift={:.1f}deg aiEnabled={}"sv,
				currentPos.x, currentPos.y, currentPos.z, anchorPos.x, anchorPos.y, anchorPos.z, posDrift, angleDrift, aiEnabled);
		}
		previousAIEnabled = aiEnabled;

		if (posDrift > kPositionDriftTolerance || angleDrift > kAngleDriftToleranceDeg) {
			logger::info("PlayerClone: reseating after drift (pos={:.1f} angle={:.1f}deg)"sv, posDrift, angleDrift);
			cloneActor->SetPosition(anchorPos, true);
			cloneActor->SetAngle({ 0.0f, 0.0f, anchorAngleZ });
		}
	}

	void PlayerClone::Despawn()
	{
		if (const auto ref = cloneRef.get()) {
			ref->Disable();
			ref->SetDelete(true);
		}
		cloneRef = {};
	}
}
