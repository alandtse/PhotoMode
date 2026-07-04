#include "LoadScreen.h"

#include "Graphics.h"
#include "Screenshots/Manager.h"

namespace LoadScreen
{
	void Manager::LoadMCMSettings(const CSimpleIniA& a_ini)
	{
		fullscreenChance = a_ini.GetLongValue("LoadScreen", "iChanceFullScreenArt", fullscreenChance);
		paintingChance = a_ini.GetLongValue("LoadScreen", "iChancePainting", paintingChance);
		debugForceType = a_ini.GetLongValue("LoadScreen", "iDebugLoadScreen", debugForceType);
	}

	void Manager::InitLoadScreenObjects()
	{
		std::vector<std::string> painting_paths;

		std::error_code ec;
		const auto      iterator = std::filesystem::directory_iterator(R"(Data\Meshes\PhotoMode\Paintings)", ec);
		if (ec) {
			logger::info("Painting assets not found, skipping ({})"sv, ec.message());
		}
		for (const auto& entry : iterator) {
			if (entry.exists()) {
				if (const auto& path = entry.path(); !path.empty() && path.extension() == ".nif") {
					auto pathStr = entry.path().string();
					painting_paths.push_back(Mesh::Sanitize(pathStr));
				}
			}
		}

		if (const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectSTAT>()) {
			paintingModels.reserve(painting_paths.size());
			for (auto& path : painting_paths) {
				if (const auto staticObj = factory->Create()) {
					staticObj->SetModel(path.data());
					paintingModels.emplace_back(staticObj);
				}
			}

			if (fullscreenModel = factory->Create(); fullscreenModel) {
				fullscreenModel->SetModel(R"(Meshes\PhotoMode\FullScreen01.nif)");
			}
		}
	}

	Type Manager::GetScreenshotModelType() const
	{
		if (debugForceType == 1) {
			return Type::kFullScreen;  // debug: always fullscreen
		}
		if (debugForceType == 2 && !paintingModels.empty()) {
			return Type::kPainting;  // debug: always painting
		}
		if (Screenshot::Manager::GetSingleton()->CanDisplayScreenshotInLoadScreen()) {
			auto rng = RNG();

			// do a coin flip if both chances are equal
			std::int32_t coinFlip = rng.generate<std::int32_t>(0, 1);

			if (coinFlip == 1) {
				if (!paintingModels.empty() && paintingChance > 0 && rng.generate<std::int32_t>(0, 100) <= paintingChance) {
					return Type::kPainting;
				}
			}

			// fallback to fullscreen
			if (fullscreenChance > 0 && rng.generate<std::int32_t>(0, 100) <= fullscreenChance) {
				return Type::kFullScreen;
			}
		}

		return Type::kNone;
	}

	RE::TESObjectSTAT* Manager::LoadScreenshotModel()
	{
		current.applied = false;  // new load screen: arm the VR deferred texture apply
		current.type = GetScreenshotModelType();

		switch (current.type) {
		case Type::kFullScreen:
			{
				current.obj = fullscreenModel;
				current.texturePath = GetScreenshotTexture();

				// skip if empty -- current.type must fall back to kNone alongside obj, or downstream
				// checks that switch on type alone (e.g. GetCameraShotPath's kFullScreen check) keep
				// treating this as a PhotoMode load screen with no model to back it.
				if (current.texturePath.empty()) {
					current.obj = nullptr;
					current.type = Type::kNone;
				}
			}
			break;
		case Type::kPainting:
			{
				current.obj = paintingModels[RNG().generate<std::size_t>(0, paintingModels.size() - 1)];  // Load random painting mesh
				current.texturePath = GetScreenshotTexture();

				// skip if empty -- see the kFullScreen case above for why type must reset too.
				if (current.texturePath.empty()) {
					current.obj = nullptr;
					current.type = Type::kNone;
				}
			}
			break;
		default:
			{
				current.obj = nullptr;
				current.texturePath.clear();
			}
			break;
		}

		if (debugForceType != 0) {
			// obj==null here means we returned no PhotoMode model -> the engine shows a VANILLA load screen
			// (e.g. a stone/statue prop with no image). A non-null model names the NIF (all paintings are
			// portrait frames: Meshes\PhotoMode\Paintings\PaintingLandscape0N.nif).
			logger::info("LoadScreen: type={} model='{}' texture='{}' -> {}"sv,
				static_cast<int>(current.type), current.obj ? current.obj->GetModel() : "",
				current.texturePath, current.obj ? "PhotoMode model" : "VANILLA fallback");
		}

		return current.obj;
	}

	std::optional<Transform> Manager::GetModelTransform() const
	{
		switch (current.type) {
		case Type::kFullScreen:
			return fullscreenTransform;
		case Type::kPainting:
			return paintingTransform;
		default:
			return std::nullopt;
		}
	}

	std::string Manager::GetScreenshotTexture() const
	{
		switch (current.type) {
		case Type::kFullScreen:
			return MANAGER(Screenshot)->GetRandomScreenshot();
		case Type::kPainting:
			return MANAGER(Screenshot)->GetRandomPainting();
		default:
			return {};
		}
	}

	const char* Manager::GetCameraShotPath(const char* a_path) const
	{
		return current.type == Type::kFullScreen ? nullptr : a_path;
	}

	bool Manager::ApplyScreenshotTexture(RE::BSGeometry* a_canvas) const
	{
		if (!a_canvas) {
			return false;
		}

		const auto effect = a_canvas->GetGeometryRuntimeData().shaderProperty;
		if (!effect) {
			return false;
		}

		const auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect.get());
		const auto material = lightingShader ? static_cast<RE::BSLightingShaderMaterial*>(lightingShader->material) : nullptr;

		if (debugForceType != 0) {
			const auto rtti = effect->GetRTTI();
			logger::info("LoadScreen: apply type={} canvas='{}' shader={} lightingShader={} material={} path='{}'"sv,
				static_cast<int>(current.type), a_canvas->name.c_str(), rtti ? rtti->name : "?",
				lightingShader != nullptr, material != nullptr, current.texturePath);
		}

		if (!lightingShader || !material) {
			return false;
		}

		if (const auto newMaterial = RE::BSLightingShaderMaterial::CreateMaterial(RE::BSShaderMaterial::Feature::kDefault)) {
			newMaterial->CopyMembers(material);
			newMaterial->ClearTextures();

			if (const auto newTextureSet = RE::BSShaderTextureSet::Create()) {
				newTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, current.texturePath.c_str());
				if (current.type == Type::kPainting) {
					newTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kNormal, R"(textures\photomode\paintings\canvaslandscape_n.dds)");
				}
				newMaterial->OnLoadTextureSet(0, newTextureSet);
			}

			// The painting canvas ships as a lit, shadow-receiving, vertex-colored, bump-mapped 3D surface,
			// so in the dim load-screen scene the screenshot won't show. The fullscreen billboard (which
			// displays fine) lacks these flags; clear them so the diffuse shows fullbright. RE: VR fullscreen
			// Canvas:0 = flags1 0x82400000 / flags2 0x8001; the painting adds kSpecular|kReceiveShadows|
			// kCastShadows|kVertexColors.
			if (current.type == Type::kPainting) {
				using F = RE::BSShaderProperty::EShaderPropertyFlag;
				lightingShader->flags.reset(F::kVertexColors, F::kReceiveShadows, F::kCastShadows, F::kSpecular);
			}

			lightingShader->SetMaterial(newMaterial, true);

			lightingShader->SetupGeometry(a_canvas);
			lightingShader->FinishSetupGeometry(a_canvas);

			newMaterial->~BSLightingShaderMaterialBase();
			RE::free(newMaterial);
			return true;
		}

		return false;
	}

	void Manager::NotifyModelReady(RE::NiNode* a_model)
	{
		if (current.type == Type::kNone || current.applied || !a_model) {
			return;
		}
		if (const auto canvas = a_model->GetObjectByName("Canvas:0")) {
			if (!ApplyScreenshotTexture(canvas->AsGeometry())) {
				return;  // geometry/shader not fully set up yet -- try again on a later ready frame
			}
			current.applied = true;

			if (current.type == Type::kPainting && REL::Module::IsVR()) {
				// The engine never resets NiAVObject::local.rotate when a new load-screen model attaches,
				// so thumbstick spin from a *previous* load screen carries over -- a painting can start
				// facing away (the frame has a solid back, so nothing shows through it). The mesh's
				// authored-default (identity) rotation also faces away from VR's camera, so reset to
				// identity plus a 180 deg yaw, with a small random jitter so loads don't look identical.
				constexpr float kMaxJitterDegrees = 20.0f;
				const float     jitter = RNG().generate<float>(-kMaxJitterDegrees, kMaxJitterDegrees);
				a_model->local.rotate = RE::NiMatrix3();
				a_model->local.rotate.MakeZRotation(RE::NI_PI + RE::deg_to_rad(jitter));
				RE::NiUpdateData updateData{};
				a_model->Update(updateData);
			}
		}
	}
}
