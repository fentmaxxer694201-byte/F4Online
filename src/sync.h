#pragma once

enum class Mode { kOff, kHost, kJoin, kEcho };

inline constexpr std::uint32_t kPuppetBaseDupPlayer = 0xFFFFFFFE;  // dev: PuppetBase=dup -> runtime copy of the player base

struct Config
{
	Mode          mode{ Mode::kOff };
	std::string   address;
	std::uint16_t port{ 27780 };
	std::string   testStart;  // dev: cell to coc into from the main menu (echo testing)
	bool          animProbe{ false };  // dev: log animation graph variables (player vs puppet)
	std::uint32_t puppetBase{ 0 };     // dev: spawn the puppet from this NPC base instead (0 = drifters)
	std::string   faceFile;            // dev (echo): send this saved look blob instead of our own
	bool          realFace{ false };   // build the peer's face (on by default; FaceMode 3 lends it to the player base briefly)
	int           faceMode{ 0 };       // 3 (default) lend face during head build; dev: 0 borrow player face, 1 no faceNPC, 2 + preset flag
	bool          building{ false };   // shared building (off: subscribing froze the game on a Sanctuary load)
};

namespace Sync
{
	void Start(const Config& a_cfg);
	void OnPreSave();  // main thread: keep the puppet out of save files
	void OnPreLoad();  // main thread: the loaded save replaces the world
	const char* CurrentStep();  // watchdog/crash log: which part of the tick is running
}
