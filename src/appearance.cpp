#include "appearance.h"

namespace
{
	// Limits keep a hostile or corrupt blob from asking for huge allocations.
	constexpr std::size_t kMaxHeadParts = 32;
	constexpr std::size_t kMaxMorphs = 512;
	constexpr std::size_t kMaxRegions = 256;
	constexpr std::size_t kMaxBones = 256;
	constexpr std::uint8_t kVersion = 1;

	class Writer
	{
	public:
		template <class T>
		void Put(const T& a_value)
		{
			const auto p = reinterpret_cast<const std::uint8_t*>(&a_value);
			bytes.insert(bytes.end(), p, p + sizeof(T));
		}
		std::vector<std::uint8_t> bytes;
	};

	class Reader
	{
	public:
		explicit Reader(const std::vector<std::uint8_t>& a_bytes) :
			bytes(a_bytes) {}

		template <class T>
		bool Get(T& a_out)
		{
			if (pos + sizeof(T) > bytes.size()) {
				return false;
			}
			std::memcpy(&a_out, bytes.data() + pos, sizeof(T));
			pos += sizeof(T);
			return true;
		}
		bool AtEnd() const { return pos == bytes.size(); }

	private:
		const std::vector<std::uint8_t>& bytes;
		std::size_t                      pos{ 0 };
	};

	bool Finite(float a_v) { return std::isfinite(a_v) && std::abs(a_v) < 1e6f; }

	template <class T>
	T* FormOrNull(std::uint32_t a_id)
	{
		return a_id ? RE::TESForm::GetFormByID<T>(a_id) : nullptr;
	}

	std::uint32_t IdOf(const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; }
}

namespace Appearance
{
	std::vector<std::uint8_t> Serialize(RE::TESNPC* a_npc, bool a_female)
	{
		Writer w;
		w.Put(kVersion);
		w.Put(IdOf(a_npc->GetFormRace()));
		w.Put(static_cast<std::uint8_t>(a_female));
		w.Put(a_npc->morphWeight.x);
		w.Put(a_npc->morphWeight.y);
		w.Put(a_npc->morphWeight.z);
		w.Put(a_npc->height);
		const auto hrd = a_npc->headRelatedData;
		w.Put(IdOf(hrd ? hrd->hairColor : nullptr));
		w.Put(IdOf(hrd ? hrd->facialHairColor : nullptr));
		w.Put(IdOf(hrd ? hrd->faceDetails : nullptr));

		const auto parts = a_npc->GetHeadParts();
		const auto nParts = static_cast<std::uint8_t>((std::min)(parts.size(), kMaxHeadParts));
		w.Put(nParts);
		for (std::size_t i = 0; i < nParts; ++i) {
			w.Put(IdOf(parts[i]));
		}

		std::uint16_t nMorph = 0;
		if (a_npc->morphSliderValues) {
			nMorph = static_cast<std::uint16_t>((std::min)(static_cast<std::size_t>(a_npc->morphSliderValues->size()), kMaxMorphs));
		}
		w.Put(nMorph);
		if (nMorph) {  // sorted: hash-map order differs between games, the bytes must not
			std::vector<std::pair<std::uint32_t, float>> sorted;
			for (const auto& [key, value] : *a_npc->morphSliderValues) {
				sorted.emplace_back(key, value);
			}
			std::ranges::sort(sorted);
			for (std::uint16_t i = 0; i < nMorph; ++i) {
				w.Put(sorted[i].first);
				w.Put(sorted[i].second);
			}
		}

		std::uint16_t nRegion = 0;
		if (a_npc->morphRegionSliderValues) {
			nRegion = static_cast<std::uint16_t>((std::min)(static_cast<std::size_t>(a_npc->morphRegionSliderValues->size()), kMaxRegions));
		}
		w.Put(nRegion);
		for (std::uint16_t i = 0; i < nRegion; ++i) {
			w.Put((*a_npc->morphRegionSliderValues)[i]);
		}

		std::uint16_t nBone = 0;
		if (a_npc->facialBoneRegionSliderValues) {
			nBone = static_cast<std::uint16_t>((std::min)(static_cast<std::size_t>(a_npc->facialBoneRegionSliderValues->size()), kMaxBones));
		}
		w.Put(nBone);
		if (nBone) {
			std::vector<std::pair<std::uint32_t, RE::BGSCharacterMorph::Transform>> sorted;
			for (const auto& [key, t] : *a_npc->facialBoneRegionSliderValues) {
				sorted.emplace_back(key, t);
			}
			std::ranges::sort(sorted, {}, &std::pair<std::uint32_t, RE::BGSCharacterMorph::Transform>::first);
			for (std::uint16_t i = 0; i < nBone; ++i) {
				w.Put(sorted[i].first);
				w.Put(sorted[i].second);
			}
		}
		return w.bytes;
	}

	RE::TESNPC* BuildNPC(const std::vector<std::uint8_t>& a_blob, int a_faceMode)
	{
		Reader         r(a_blob);
		std::uint8_t   version = 0, female = 0, nParts = 0;
		std::uint32_t  raceID = 0, hairID = 0, facialHairID = 0, detailsID = 0;
		RE::NiPoint3   weight;
		float          height = 1.0f;
		if (!r.Get(version) || version != kVersion || !r.Get(raceID) || !r.Get(female) || female > 1 ||
			!r.Get(weight.x) || !r.Get(weight.y) || !r.Get(weight.z) || !r.Get(height) ||
			!Finite(weight.x) || !Finite(weight.y) || !Finite(weight.z) || !Finite(height) || height <= 0.0f || height > 3.0f ||
			!r.Get(hairID) || !r.Get(facialHairID) || !r.Get(detailsID) || !r.Get(nParts) || nParts > kMaxHeadParts) {
			REX::WARN("Look: bad header");
			return nullptr;
		}
		const auto race = FormOrNull<RE::TESRace>(raceID);
		if (!race) {
			REX::WARN("Look: race {:08X} not found", raceID);
			return nullptr;
		}
		std::vector<RE::BGSHeadPart*> parts;
		for (std::uint8_t i = 0; i < nParts; ++i) {
			std::uint32_t id = 0;
			if (!r.Get(id)) {
				REX::WARN("Look: truncated head parts");
				return nullptr;
			}
			if (const auto part = FormOrNull<RE::BGSHeadPart>(id)) {
				parts.push_back(part);
			} else {
				REX::WARN("Look: head part {:08X} not found, skipped", id);
			}
		}
		std::uint16_t nMorph = 0;
		if (!r.Get(nMorph) || nMorph > kMaxMorphs) {
			REX::WARN("Look: bad morph count");
			return nullptr;
		}
		std::vector<std::pair<std::uint32_t, float>> morphs(nMorph);
		for (auto& [key, value] : morphs) {
			if (!r.Get(key) || !r.Get(value) || !Finite(value)) {
				REX::WARN("Look: bad morph");
				return nullptr;
			}
		}
		std::uint16_t nRegion = 0;
		if (!r.Get(nRegion) || nRegion > kMaxRegions) {
			REX::WARN("Look: bad region count");
			return nullptr;
		}
		std::vector<float> regions(nRegion);
		for (auto& v : regions) {
			if (!r.Get(v) || !Finite(v)) {
				REX::WARN("Look: bad region value");
				return nullptr;
			}
		}
		std::uint16_t nBone = 0;
		if (!r.Get(nBone) || nBone > kMaxBones) {
			REX::WARN("Look: bad bone count");
			return nullptr;
		}
		std::vector<std::pair<std::uint32_t, RE::BGSCharacterMorph::Transform>> bones(nBone);
		for (auto& [key, t] : bones) {
			if (!r.Get(key) || !r.Get(t) || !Finite(t.position.x) || !Finite(t.position.y) || !Finite(t.position.z) ||
				!Finite(t.rotation.x) || !Finite(t.rotation.y) || !Finite(t.rotation.z) ||
				!Finite(t.scale.x) || !Finite(t.scale.y) || !Finite(t.scale.z)) {
				REX::WARN("Look: bad bone");
				return nullptr;
			}
		}
		if (!r.AtEnd()) {
			REX::WARN("Look: trailing bytes");
			return nullptr;
		}

		// Start from a copy of the player base (its face is built live), then give the copy its own
		// containers before writing: Copy() shares them with the player, and writing into those would
		// change the local player's face too.
		const auto player = RE::TESForm::GetFormByID<RE::TESNPC>(0x7);
		const auto factory = RE::ConcreteFormFactory<RE::TESNPC>::GetFormFactory();
		const auto npc = player && factory ? factory->Create() : nullptr;
		if (!npc) {
			REX::ERROR("Look: couldn't create an NPC");
			return nullptr;
		}
		npc->Copy(player);
		// Copy() links the copy's face to the player (faceNPC), so both shared one generated face: building
		// the peer's look put it on the local player too. The copy must own its face.
		REX::INFO("Look: copy's faceNPC was {:08X}; player's is {:08X}", npc->faceNPC ? npc->faceNPC->GetFormID() : 0,
			player->faceNPC ? player->faceNPC->GetFormID() : 0);
		if (a_faceMode >= 1) {
			npc->faceNPC = nullptr;
		}
		if (a_faceMode == 2) {  // chargen preset faces are built live by the engine (no FaceGen files)
			npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kIsChargenFacePreset);
		}

		npc->SetFormRace(race);
		npc->originalRace = race;  // so GetHeadParts uses our own list, not the player's alternate one
		if (female) {
			npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
		} else {
			npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
		}
		npc->morphWeight = weight;
		npc->height = height;

		const auto hrd = static_cast<RE::TESNPC::HeadRelatedData*>(RE::malloc(sizeof(RE::TESNPC::HeadRelatedData)));
		hrd->hairColor = FormOrNull<RE::BGSColorForm>(hairID);
		hrd->facialHairColor = FormOrNull<RE::BGSColorForm>(facialHairID);
		hrd->faceDetails = FormOrNull<RE::BGSTextureSet>(detailsID);
		npc->headRelatedData = hrd;

		npc->headParts = parts.empty() ? nullptr : static_cast<RE::BGSHeadPart**>(RE::malloc(parts.size() * sizeof(RE::BGSHeadPart*)));
		std::ranges::copy(parts, npc->headParts);
		npc->numHeadParts = static_cast<std::int8_t>(parts.size());

		using MorphMap = RE::BSTHashMap<std::uint32_t, float>;
		auto morphMap = new (RE::malloc(sizeof(MorphMap))) MorphMap();
		for (const auto& [key, value] : morphs) {
			morphMap->insert({ key, value });
		}
		npc->morphSliderValues = morphMap;

		// The engine indexes region sliders by the race's region count; a character who never touched them
		// sends 0 values (crashed the game). Pad with 0 (neutral) / trim to the local player's count.
		const std::size_t wantRegions = player->morphRegionSliderValues ? player->morphRegionSliderValues->size() : regions.size();
		regions.resize(wantRegions, 0.0f);
		auto regionArr = new RE::BSTArray<float>();
		for (const auto v : regions) {
			regionArr->push_back(v);
		}
		npc->morphRegionSliderValues = regionArr;

		using BoneMap = RE::BSTHashMap<std::uint32_t, RE::BGSCharacterMorph::Transform>;
		auto boneMap = new (RE::malloc(sizeof(BoneMap))) BoneMap();
		for (const auto& [key, t] : bones) {
			boneMap->insert({ key, t });
		}
		npc->facialBoneRegionSliderValues = boneMap;

		REX::INFO("Look: built NPC {:08X} ({}, {} head parts, {} morphs, {} regions, {} bones, weight {:.2f}/{:.2f}/{:.2f})",
			npc->GetFormID(), female ? "female" : "male", parts.size(), morphs.size(), regions.size(), bones.size(), weight.x, weight.y, weight.z);
		return npc;
	}

	void DumpHead(RE::Actor* a_actor)
	{
		std::string out;
		int         geometry = 0;
		std::function<int(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* a_obj, int a_depth) -> int {
			if (!a_obj || a_depth > 14) {
				return 0;
			}
			int geo = a_obj->IsGeometry() ? 1 : 0;
			if (const auto node = a_obj->IsNode()) {
				for (const auto& child : node->children) {
					geo += walk(child.get(), a_depth + 1);
				}
			}
			const std::string name = a_obj->GetName().c_str() ? a_obj->GetName().c_str() : "";
			if (name.find("Face") != std::string::npos || name.find("Head") != std::string::npos || name.find("Hair") != std::string::npos ||
				name.find("Beard") != std::string::npos || name.find("Eyes") != std::string::npos) {
				out += std::format(" [{} geo={}]", name, geo);
			}
			return geo;
		};
		geometry = walk(a_actor->Get3D(false), 0);  // third-person model (the first-person one has no head)
		REX::INFO("Head dump {:08X}: total geometry {} |{}", a_actor->GetFormID(), geometry, out.empty() ? " no head/face/hair nodes" : out);
	}

	void SwapFace(RE::TESNPC* a_a, RE::TESNPC* a_b)
	{
		std::swap(a_a->headParts, a_b->headParts);
		std::swap(a_a->numHeadParts, a_b->numHeadParts);
		std::swap(a_a->morphSliderValues, a_b->morphSliderValues);
		std::swap(a_a->morphRegionSliderValues, a_b->morphRegionSliderValues);
		std::swap(a_a->facialBoneRegionSliderValues, a_b->facialBoneRegionSliderValues);
		std::swap(a_a->headRelatedData, a_b->headRelatedData);
		std::swap(a_a->morphWeight, a_b->morphWeight);
	}

	void SelfTest(RE::TESNPC* a_npc, bool a_female)
	{
		const auto blob = Serialize(a_npc, a_female);
		const auto built = BuildNPC(blob);
		const bool ok = built && Serialize(built, a_female) == blob;
		REX::INFO("Look self-test: {} ({} bytes)", ok ? "round trip OK" : "MISMATCH", blob.size());
	}
}
