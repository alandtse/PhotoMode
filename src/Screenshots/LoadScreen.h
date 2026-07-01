#pragma once

namespace LoadScreen
{
	enum class Type : std::uint8_t
	{
		kNone,
		kFullScreen,
		kPainting
	};

	struct Transform
	{
		float        scale{ 1.0f };
		RE::NiPoint3 rotationalOffset{};
		RE::NiPoint3 translateOffset{};
	};

	class Manager final : public REX::Singleton<Manager>
	{
	public:
		void LoadMCMSettings(const CSimpleIniA& a_ini);
		void InitLoadScreenObjects();

		RE::TESObjectSTAT* LoadScreenshotModel();

		std::optional<Transform> GetModelTransform() const;
		const char*              GetCameraShotPath(const char* a_path) const;

		void ApplyScreenshotTexture(RE::BSGeometry* a_canvas) const;

		// VR streams the load-screen model in asynchronously, after InitLoadScreen3D's first setup call;
		// the AdvanceMovie hook calls this each frame with the model NIF root once it appears, so the
		// texture lands on Canvas:0 exactly once per load screen (a no-op on SE/AE, which apply inline).
		void NotifyModelReady(RE::NiNode* a_model);

	private:
		Type        GetScreenshotModelType() const;
		std::string GetScreenshotTexture() const;

		// members
		std::int32_t fullscreenChance{ 50 };
		std::int32_t paintingChance{ 50 };
		std::int32_t debugForceType{ 0 };  // 0 = normal, 1 = always fullscreen, 2 = always painting (testing)

		std::vector<RE::TESObjectSTAT*> paintingModels{};
		Transform                       paintingTransform{ 0.6f, RE::NiPoint3(), RE::NiPoint3() };
		RE::TESObjectSTAT*              fullscreenModel{};
		Transform                       fullscreenTransform{ 2.0f, RE::NiPoint3(), RE::NiPoint3(-45.0, 0, 0) };

		struct
		{
			RE::TESObjectSTAT* obj{};
			Type               type{ Type::kNone };
			std::string        texturePath{};
			bool               applied{ false };  // VR: texture already landed on this load screen's canvas

		} current;
	};
}
