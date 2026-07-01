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

		// combatStyle isn't covered by any TEMPLATE_USE_FLAG we borrow from the Mannequin, so it's also
		// null on a fresh base -- the background combat-evaluation job (queued on interaction/activation)
		// dereferences it and CTDs. StopCombat() at spawn keeps the clone out of actual combat regardless
		// of whose style this is.
		a_clone->SetCombatStyle(a_playerBase->GetCombatStyle());

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

	void PlayerClone::Spawn()
	{
		if (IsSpawned()) {
			return;
		}
		poseApplied = false;
		faceReset = false;

		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto playerBase = player ? player->GetActorBase() : nullptr;
		if (!playerBase) {
			return;
		}
		spawnPos = player->GetPosition();  // pin the clone here; PlaceObjectAtMe shoves it out of the capsule

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
			} else {
				logger::warn("PlayerClone: vanilla Mannequin base (0x89A85) not found; clone may be inert"sv);
			}
		}
		UpdateAppearance(cloneBase, playerBase);

		const auto ref = player->PlaceObjectAtMe(cloneBase, true);
		if (!ref) {
			logger::error("PlayerClone: PlaceObjectAtMe failed"sv);
			return;
		}
		cloneRef = ref->CreateRefHandle();

		ref->data.angle = { 0.0f, 0.0f, player->GetAngleZ() };
		if (const auto cloneActor = ref->As<RE::Actor>()) {
			// AI stays enabled so the clone gains a high process (facegen data + idle playback); the
			// Character tab's do-nothing package keeps it standing still.
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

		if (poseApplied) {
			return;
		}

		if (!player) {
			return;
		}

		// Wait for the clone's 3D (head + body) to stream in — it loads a few frames after
		// PlaceObjectAtMe. The clone poses via its own idle animation (AI is enabled), so there is NO
		// manual bone-copy: copying the player's bone locals (including the root/COM nodes) offset the
		// clone's skeleton from its actor position, which mislocated effect art and made the Transforms
		// sliders apply incorrectly.
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

		// PlaceObjectAtMe spawns the clone overlapping the player capsule, so the solver shoves it out
		// (it lands in front). Suspend sim just long enough to teleport it back without being shoved
		// again, then re-enable it: kNoSim disables the character controller's entire simulation pass
		// (ground support, gravity-on-ground), not just movement, so leaving it off let the clone's own
		// idle animation and HIGGS's ragdoll-bone grabbing desync from the ground with nothing
		// reconciling position/orientation against the terrain -- it needs normal sim to stay upright.
		if (const auto charController = cloneActor->GetCharController()) {
			charController->flags.set(RE::CHARACTER_FLAGS::kNoSim);
			cloneActor->SetPosition(spawnPos, true);
			charController->flags.reset(RE::CHARACTER_FLAGS::kNoSim);
		} else {
			cloneActor->SetPosition(spawnPos, true);
		}

		// Now that the clone's 3D exists, replay the player's active-effect visuals and the
		// readied-spell charge art onto it.
		CopyVisualEffects(cloneActor, player);
		CopyHandMagic(cloneActor, player);

		poseApplied = true;
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
