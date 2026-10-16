#pragma once

// A player's chargen look (race, sex, weight, head parts, colors, face sliders and bones) as bytes,
// and a runtime NPC built from them. Skin tints are not included yet (the copy keeps the local
// player's tints).
namespace Appearance
{
	std::vector<std::uint8_t> Serialize(RE::TESNPC* a_npc, bool a_female);

	// Validates a_blob fully; returns nullptr (and logs why) if anything is off.
	RE::TESNPC* BuildNPC(const std::vector<std::uint8_t>& a_blob, int a_faceMode = 1);

	// Dev: log head/face/hair nodes of an actor's 3D with geometry counts (is a head actually built?).
	void DumpHead(RE::Actor* a_actor);

	// Dev (FaceMode 3): swap the face fields of two NPC bases (pointers/values only, no allocation).
	void SwapFace(RE::TESNPC* a_a, RE::TESNPC* a_b);

	// Assert-based round trip: Serialize -> BuildNPC -> Serialize must match. Logs the result.
	void SelfTest(RE::TESNPC* a_npc, bool a_female);
}
