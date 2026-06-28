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

		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto playerBase = player ? player->GetActorBase() : nullptr;
		if (!playerBase) {
			return;
		}

		if (!cloneBase) {
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESNPC>();
			cloneBase = factory ? factory->Create() : nullptr;
			if (!cloneBase) {
				logger::error("PlayerClone: failed to create clone base"sv);
				return;
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
			// Make it an inert display dummy: a freshly created NPC base has no combat/idle
			// data, so the AI update (RandomlyPlaySpecialIdles) dereferences null and crashes.
			// Disable AI before the next actor-update tick so it is never processed.
			cloneActor->EnableAI(false);
			cloneActor->StopCombat();
			CopyWornEquipment(cloneActor, player);
			cloneActor->DoReset3D(true);  // rebuild the biped so the equipped gear renders
		}
		logger::info("PlayerClone: spawned photo clone"sv);
	}

	void PlayerClone::ApplyPose()
	{
		if (poseApplied) {
			return;
		}

		const auto cloneRefPtr = cloneRef.get();
		const auto cloneActor = cloneRefPtr ? cloneRefPtr->As<RE::Actor>() : nullptr;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!cloneActor || !player) {
			return;
		}

		// Both are third-person actor skeletons, so the bone names match. The player is frozen
		// (PhotoMode pauses time on activate), so reading it now reproduces the entry pose even
		// though the clone's 3D streams in a few frames after PlaceObjectAtMe.
		const auto cloneRoot = cloneActor->Get3D(false);
		const auto playerRoot = player->Get3D(false);
		if (!cloneRoot || !playerRoot) {
			return;  // clone 3D not loaded yet — try again next frame
		}

		std::unordered_map<std::string, RE::NiTransform> pose;
		VisitNodes(playerRoot, [&](RE::NiAVObject* a_node) {
			if (const auto* name = a_node->name.c_str(); name && *name) {
				pose.insert_or_assign(name, a_node->local);
			}
		});

		std::size_t matched = 0;
		VisitNodes(cloneRoot, [&](RE::NiAVObject* a_node) {
			if (const auto* name = a_node->name.c_str(); name && *name) {
				if (const auto it = pose.find(name); it != pose.end()) {
					a_node->local = it->second;
					++matched;
				}
			}
		});

		RE::NiUpdateData updateData{};
		cloneRoot->Update(updateData);

		// Now that the clone's 3D exists, replay the player's active-effect visuals and the
		// readied-spell charge art onto it.
		CopyVisualEffects(cloneActor, player);
		CopyHandMagic(cloneActor, player);

		poseApplied = true;
		logger::info("PlayerClone: applied pose ({} of {} bones)"sv, matched, pose.size());
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
