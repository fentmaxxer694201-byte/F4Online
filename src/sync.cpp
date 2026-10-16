#include "sync.h"

#include "appearance.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#undef ERROR  // wingdi.h macro clashes with REX::ERROR

namespace
{
	// Wire format. Fixed size, little-endian (both ends are x64 Windows).
	constexpr std::uint32_t kMagic = 0x374F3446;  // "F4O7" (0.11: workshop builds; gifts must not reach builds that drop them)
	enum class Type : std::uint8_t { kState = 1, kKill = 2, kDamage = 3, kXP = 4, kName = 5, kGive = 6, kLook = 7, kFace = 8, kBuild = 9 };

#pragma pack(push, 1)
	struct Packet
	{
		std::uint32_t magic;
		Type          type;
		std::uint32_t seq;
		float         x, y, z, yaw;
		std::uint32_t area;      // interior cell form ID, or worldspace form ID outdoors
		std::uint8_t  interior;  // 1 if area is a cell
		float         speed;     // sender's animation graph "Speed" (locomotion blend)
		float         dir;       // sender's animation graph "Direction" (0..1 around the facing)
		std::uint8_t  sprint;    // graph "IsSprinting"
		std::uint8_t  drawn;     // weapon out
	};

	struct KillPacket
	{
		std::uint32_t magic;
		Type          type;
		RE::TESFormID actor;  // a world-placed actor: same ID in every copy of the game
	};

	struct DamagePacket
	{
		std::uint32_t magic;
		Type          type;
		RE::TESFormID actor;
		float         amount;  // health the sender's hit took off (a delta, so both players' hits add up)
	};
#pragma pack(pop)

	struct XPPacket
	{
		std::uint32_t magic;
		Type          type;
		float         amount;  // XP the sender just earned
	};

	struct NamePacket
	{
		std::uint32_t magic;
		Type          type;
		char          name[32];  // sender's character name, NUL-terminated
	};

	struct GivePacket
	{
		std::uint32_t magic;
		Type          type;
		RE::TESFormID item;   // base item form (same in both games with the same mods)
		std::int32_t  count;
	};

	constexpr int kMaxWorn = 24;
	struct LookPacket
	{
		std::uint32_t magic;
		Type          type;
		std::uint8_t  female;
		std::uint8_t  count;           // used entries of worn
		RE::TESFormID worn[kMaxWorn];  // armor/clothing in every body slot + the weapon in hand
	};

	constexpr std::size_t kFaceChunk = 1024;
	constexpr int         kMaxFaceChunks = 16;
	struct FacePacket
	{
		std::uint32_t magic;
		Type          type;
		std::uint16_t version;  // hash of the whole blob: chunks of different versions never mix
		std::uint8_t  index;
		std::uint8_t  total;
		std::uint16_t len;
		std::uint8_t  data[kFaceChunk];
	};

	enum class BuildOp : std::uint8_t { kPlace = 1, kMove = 2, kRemove = 3 };
	struct BuildPacket
	{
		std::uint32_t magic;
		Type          type;
		BuildOp       op;
		std::uint32_t buildID;     // sender-chosen; names the object across both games
		RE::TESFormID base;        // what was built
		float         x, y, z;     // where
		float         ax, ay, az;  // rotation
		std::uint32_t area;        // interior cell or worldspace
		std::uint8_t  interior;
	};

	constexpr std::size_t kRecvBuf = 2048;
	static_assert(sizeof(LookPacket) <= kRecvBuf && sizeof(Packet) <= kRecvBuf && sizeof(FacePacket) <= kRecvBuf,
		"receive buffer in ReceiveAll must hold every packet");

	struct Drop
	{
		RE::TESFormID ref;    // the item lying in the world after the drop
		RE::TESFormID item;
		std::int32_t  count;
	};

	struct HitCheck
	{
		RE::TESFormID actor;
		float         healthAtHit;
		float         reported;  // hitData.healthDamage
	};

	struct RemoteState
	{
		Packet                                pkt{};
		std::chrono::steady_clock::time_point at{};
		bool                                  valid{ false };
		float                                 vx{ 0 }, vy{ 0 }, vz{ 0 };  // velocity from the last two packets (units/s)
	};

	constexpr auto       kTick = std::chrono::milliseconds(16);  // ~60 Hz puppet smoothing
	constexpr int        kSendEvery = 3;                         // state packets at ~20 Hz
	constexpr auto       kStale = std::chrono::seconds(3);
	constexpr float      kEchoOffset = 150.0f;  // echo puppet stands ~2 m beside you
	// Plain human with full facegen and no scripts (GoodneighborDrifterTemplate00). Clones of the
	// player base (0x7) have no body: the player's look is built at chargen, not stored on the base.
	constexpr RE::TESFormID kPuppetBase = 0x0002331F;
	constexpr RE::TESFormID kOldPuppetBase = 0x00000007;  // early builds; may linger in saves
	constexpr RE::TESFormID kPuppetBaseFemale = 0x000C3C53;  // CambridgeDrifterFem: same kind of plain, script-free human
	constexpr RE::TESFormID kPlayerAllyFaction = 0x00106C30;

	Config            g_cfg;
	SOCKET            g_sock = INVALID_SOCKET;
	sockaddr_in       g_peer{};
	bool              g_havePeer = false;
	std::mutex        g_lock;  // guards g_remote and g_peer/g_havePeer
	RemoteState       g_remote;
	std::uint32_t     g_seq = 0;
	RE::ObjectRefHandle g_puppet;
	bool              g_puppetPeaceful = false;  // ally/ghost commands applied to the current puppet
	std::chrono::steady_clock::time_point g_spawnedAt{};
	std::jthread      g_thread;
	// Watchdog: which step of Tick is running, and when Tick last finished (steady ns).
	std::atomic<const char*>  g_step{ "idle" };
	std::atomic<std::int64_t> g_tickDoneNs{ 0 };
	std::vector<RE::TESFormID>        g_pendingKills;   // received, applied on the main thread (guarded by g_lock)
	std::vector<DamagePacket>         g_pendingDamage;  // received, applied on the main thread (guarded by g_lock)
	std::vector<HitCheck>             g_hitChecks;      // our hits, measured next tick (main thread only)
	std::unordered_map<RE::TESFormID, float> g_health;  // nearby actors' health as of last tick (main thread only)
	RE::NiPoint3                      g_shownPos;       // where we've drawn the puppet (smoothed toward the target)
	float                             g_shownYaw = 0.0f;
	bool                              g_shownValid = false;
	std::string                       g_remoteName;     // peer's character name (guarded by g_lock)
	std::string                       g_shownName;      // name currently on the puppet (main thread only)
	std::vector<Drop>                 g_drops;          // our drops next to the puppet, handled next tick (main thread only)
	std::vector<GivePacket>           g_pendingGifts;   // received items (guarded by g_lock)
	LookPacket                        g_remoteLook{};   // peer's sex + worn items (guarded by g_lock)
	std::vector<RE::TESFormID>        g_shownLook;      // worn list currently on the puppet (main thread only)
	std::uint16_t                     g_faceAssembling = 0;                  // version being received (guarded by g_lock)
	std::vector<std::vector<std::uint8_t>> g_faceParts;                      // its chunks (guarded by g_lock)
	std::uint16_t                     g_faceReadyVersion = 0;                // complete blob (guarded by g_lock)
	std::vector<std::uint8_t>         g_faceReady;
	std::uint16_t                     g_faceBuiltVersion = 0;                // main thread only
	RE::TESNPC*                       g_faceNPC = nullptr;                   // peer's look; puppet base when set
	std::unordered_map<RE::TESFormID, std::uint32_t> g_myBuilds;             // our placed ref -> buildID (main thread)
	std::uint32_t                     g_buildCounter = 0;
	std::vector<BuildPacket>          g_pendingBuilds;                       // received (guarded by g_lock)
	std::unordered_map<std::uint32_t, RE::ObjectRefHandle> g_mirrors;        // buildID -> our copy (main thread)
	std::chrono::steady_clock::time_point g_stripAt{};  // when we asked the console to strip the puppet
	bool                              g_stripped = false;  // strip done for the current want-list; dress next
	int                               g_dressTries = 0;
	constexpr float                   kGiveRange = 200.0f;  // drop within ~2.5 m of the puppet to give
	float                             g_pendingXP = 0.0f;  // received, awarded on the main thread (guarded by g_lock)
	float                             g_xpSeen = -1.0f;    // our XP last tick; -1 = unknown (after a load)
	std::unordered_set<RE::TESFormID> g_remoteKills;    // killed because the peer said so: don't echo back
	std::unordered_set<RE::TESFormID> g_strayTried;     // stray puppets we already tried to remove

	bool Finite(const Packet& p)
	{
		return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.yaw) &&
		       std::abs(p.x) < 1e7f && std::abs(p.y) < 1e7f && std::abs(p.z) < 1e7f &&
		       std::isfinite(p.speed) && p.speed >= 0.0f && p.speed < 5000.0f && std::isfinite(p.dir);
	}

	std::string Addr(const sockaddr_in& a)
	{
		char buf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
		return std::format("{}:{}", buf, ntohs(a.sin_port));
	}

	void ReceiveAll()
	{
		for (;;) {
			char        buf[kRecvBuf];
			sockaddr_in from{};
			int         fromLen = sizeof(from);
			const int   n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (n == SOCKET_ERROR) {
				return;  // WSAEWOULDBLOCK: queue drained (other errors: try again next tick)
			}
			Packet     p{};
			KillPacket k{};
			const bool isState = n == sizeof(p) && (std::memcpy(&p, buf, sizeof(p)), p.magic == kMagic && p.type == Type::kState && Finite(p));
			const bool isKill = n == sizeof(k) && (std::memcpy(&k, buf, sizeof(k)), k.magic == kMagic && k.type == Type::kKill && (k.actor >> 24) != 0xFF);
			DamagePacket dmg{};
			const bool   isDamage = n == sizeof(dmg) && (std::memcpy(&dmg, buf, sizeof(dmg)), dmg.magic == kMagic && dmg.type == Type::kDamage &&
			                                                (dmg.actor >> 24) != 0xFF && std::isfinite(dmg.amount) && dmg.amount > 0.0f && dmg.amount < 100000.0f);
			XPPacket   xp{};
			const bool isXP = n == sizeof(xp) && (std::memcpy(&xp, buf, sizeof(xp)), xp.magic == kMagic && xp.type == Type::kXP &&
			                                         std::isfinite(xp.amount) && xp.amount > 0.0f && xp.amount < 100000.0f);
			NamePacket nm{};
			const bool isName = n == sizeof(nm) && (std::memcpy(&nm, buf, sizeof(nm)), nm.magic == kMagic && nm.type == Type::kName) &&
			                    std::memchr(nm.name, 0, sizeof(nm.name)) != nullptr && nm.name[0] != 0 &&
			                    std::all_of(nm.name, nm.name + std::strlen(nm.name), [](char c) { return static_cast<unsigned char>(c) >= 0x20; });
			GivePacket gv{};
			const bool isGive = n == sizeof(gv) && (std::memcpy(&gv, buf, sizeof(gv)), gv.magic == kMagic && gv.type == Type::kGive &&
			                                         (gv.item >> 24) != 0xFF && gv.count > 0 && gv.count <= 100000);
			LookPacket lk{};
			const bool isLook = n == sizeof(lk) && (std::memcpy(&lk, buf, sizeof(lk)), lk.magic == kMagic && lk.type == Type::kLook &&
			                                         lk.count <= kMaxWorn && lk.female <= 1);
			FacePacket fc{};
			const bool isFace = n >= static_cast<int>(offsetof(FacePacket, data)) &&
			                    (std::memcpy(&fc, buf, (std::min)(static_cast<std::size_t>(n), sizeof(fc))), fc.magic == kMagic && fc.type == Type::kFace &&
			                        fc.total > 0 && fc.total <= kMaxFaceChunks && fc.index < fc.total && fc.len <= kFaceChunk &&
			                        n == static_cast<int>(offsetof(FacePacket, data) + fc.len));
			BuildPacket bp{};
			const bool  isBuild = n == sizeof(bp) && (std::memcpy(&bp, buf, sizeof(bp)), bp.magic == kMagic && bp.type == Type::kBuild &&
			                                             (bp.op == BuildOp::kPlace || bp.op == BuildOp::kMove || bp.op == BuildOp::kRemove) &&
			                                             std::isfinite(bp.x) && std::isfinite(bp.y) && std::isfinite(bp.z) &&
			                                             std::isfinite(bp.ax) && std::isfinite(bp.ay) && std::isfinite(bp.az) &&
			                                             std::abs(bp.x) < 1e7f && std::abs(bp.y) < 1e7f && std::abs(bp.z) < 1e7f && bp.interior <= 1);
			if (!isState && !isKill && !isDamage && !isXP && !isName && !isGive && !isLook && !isFace && !isBuild) {
				continue;  // not ours or malformed: drop
			}
			std::scoped_lock l(g_lock);
			if (!g_havePeer) {  // host: first sender becomes the guest
				g_peer = from;
				g_havePeer = true;
				REX::INFO("Peer connected from {}", Addr(from));
			} else if (from.sin_addr.s_addr != g_peer.sin_addr.s_addr || from.sin_port != g_peer.sin_port) {
				continue;  // step 1 is two players: ignore everyone else
			}
			if (isKill) {
				g_pendingKills.push_back(k.actor);
				continue;
			}
			if (isDamage) {
				g_pendingDamage.push_back(dmg);
				continue;
			}
			if (isXP) {
				g_pendingXP += xp.amount;
				continue;
			}
			if (isName) {
				g_remoteName = nm.name;
				continue;
			}
			if (isGive) {
				g_pendingGifts.push_back(gv);
				continue;
			}
			if (isLook) {
				g_remoteLook = lk;
				continue;
			}
			if (isBuild) {
				g_pendingBuilds.push_back(bp);
				continue;
			}
			if (isFace) {
				if (fc.version != g_faceAssembling || g_faceParts.size() != fc.total) {
					g_faceAssembling = fc.version;
					g_faceParts.assign(fc.total, {});
				}
				g_faceParts[fc.index].assign(fc.data, fc.data + fc.len);
				if (fc.version != g_faceReadyVersion && std::ranges::all_of(g_faceParts, [](const auto& c) { return !c.empty(); })) {
					g_faceReady.clear();
					for (const auto& c : g_faceParts) {
						g_faceReady.insert(g_faceReady.end(), c.begin(), c.end());
					}
					g_faceReadyVersion = fc.version;
				}
				continue;
			}
			if (g_remote.valid && static_cast<std::int32_t>(p.seq - g_remote.pkt.seq) <= 0) {
				continue;  // old or duplicate
			}
			const auto now = std::chrono::steady_clock::now();
			RemoteState next{ p, now, true };
			const float dt = std::chrono::duration<float>(now - g_remote.at).count();
			if (g_remote.valid && dt > 0.02f && dt < 0.5f) {  // blend with the old estimate to calm network jitter
				next.vx = 0.5f * g_remote.vx + 0.5f * (p.x - g_remote.pkt.x) / dt;
				next.vy = 0.5f * g_remote.vy + 0.5f * (p.y - g_remote.pkt.y) / dt;
				next.vz = 0.5f * g_remote.vz + 0.5f * (p.z - g_remote.pkt.z) / dt;
			}
			g_remote = next;
		}
	}

	void DespawnPuppet()
	{
		if (auto ref = g_puppet.get()) {
			ref->Disable();
			ref->SetDelete(true);
			REX::INFO("Puppet removed");
		}
		g_puppet.reset();
		g_shownValid = false;
		g_shownName.clear();
	}

	// A runtime copy of the player's NPC base (face built live, like the player's). Phase B will write the
	// peer's appearance into a copy like this.
	RE::TESNPC* DuplicatePlayerBase()
	{
		static RE::TESNPC* dup = nullptr;
		const auto         player = RE::TESForm::GetFormByID<RE::TESNPC>(0x7);
		const auto         factory = RE::ConcreteFormFactory<RE::TESNPC>::GetFormFactory();
		if (!dup && player && factory) {
			dup = factory->Create();
			if (dup) {
				dup->Copy(player);
				REX::INFO("Created runtime NPC {:08X} as a copy of the player base", dup->GetFormID());
			}
		}
		return dup;
	}

	bool g_faceSwapped = false;  // FaceMode 3: the player base currently holds the peer's face fields
	std::chrono::steady_clock::time_point g_noSwapUntil{};  // no face lending right after a save starts

	RE::Actor* SpawnPuppet(const Packet& p, RE::TESObjectCELL* cell)
	{
		bool female;
		{
			std::scoped_lock l(g_lock);
			female = g_remoteLook.female != 0;
		}
		const auto baseID = g_cfg.puppetBase ? g_cfg.puppetBase : female ? kPuppetBaseFemale : kPuppetBase;
		const auto base = !g_cfg.puppetBase && g_faceNPC ? g_faceNPC :
		                  baseID == kPuppetBaseDupPlayer ? DuplicatePlayerBase() :
		                                                   RE::TESForm::GetFormByID<RE::TESNPC>(baseID);
		if (!base) {
			REX::ERROR("Puppet base NPC {:08X} not found", baseID);
			return nullptr;
		}
		RE::NEW_REFR_DATA d;
		d.location = { p.x, p.y, p.z };
		d.direction = { 0.0f, 0.0f, p.yaw };
		d.object = base;
		d.clearStillLoadingFlag = true;  // otherwise the actor stays hidden and script-unbindable
		if (cell->IsInterior()) {
			d.interior = cell;
		} else {
			d.world = cell->worldSpace;
		}
		if (g_cfg.faceMode == 3 && base == g_faceNPC && !g_faceSwapped && std::chrono::steady_clock::now() < g_noSwapUntil) {
			return nullptr;  // a save just started; spawn (and lend the face) a moment later
		}
		if (g_cfg.faceMode == 3 && base == g_faceNPC && !g_faceSwapped) {  // lend the peer's face to the player base
			Appearance::SwapFace(RE::TESForm::GetFormByID<RE::TESNPC>(0x7), g_faceNPC);
			g_faceSwapped = true;
			REX::INFO("FaceMode 3: player base now holds the peer's face fields");
		}
		g_puppet = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(d);
		const auto ref = g_puppet.get();
		if (!ref) {
			REX::ERROR("Puppet spawn failed");
			return nullptr;
		}
		g_puppetPeaceful = false;
		g_spawnedAt = std::chrono::steady_clock::now();
		g_shownLook.clear();
		REX::INFO("Puppet {:08X} spawned at ({:.0f}, {:.0f}, {:.0f})", ref->GetFormID(), p.x, p.y, p.z);
		return ref->As<RE::Actor>();
	}

	// Console commands run while a terminal, Pip-Boy, dialogue, lockpick or other menu is up can
	// freeze that menu (a terminal got stuck this way), so all our console work waits for gameplay.
	bool MenuOpen()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return true;
		}
		if (ui->menuMode > 0) {
			return true;
		}
		for (const auto name : { "TerminalMenu", "TerminalHolotapeMenu", "PipboyMenu", "DialogueMenu", "LockpickingMenu", "BookMenu",
				 "ContainerMenu", "BarterMenu", "ExamineMenu", "WorkshopMenu", "Console", "LooksMenu", "LevelUpMenu" }) {
			if (ui->GetMenuOpen(name)) {
				return true;
			}
		}
		return false;
	}

	// The puppet keeps its own AI (Papyrus EnableAI crashed us), so make it a harmless ally instead:
	// otherwise companions pick fights with it and it fights back. Console works on loaded refs.
	void MakePuppetPeaceful(RE::Actor* a_actor)
	{
		if (g_puppetPeaceful || !a_actor->Get3D() || MenuOpen()) {
			return;
		}
		const auto id = a_actor->GetFormID();
		for (const auto& cmd : { std::format("{:08X}.RemoveFromAllFactions", id),
				 std::format("{:08X}.AddToFaction {:08X} 1", id, kPlayerAllyFaction),
				 std::format("{:08X}.SetAV Aggression 0", id),
				 std::format("{:08X}.SetGhost 1", id),
				 std::format("{:08X}.StopCombat", id) }) {
			RE::Console::ExecuteCommand(cmd.c_str());
		}
		g_puppetPeaceful = true;
		REX::INFO("Puppet {:08X} made a peaceful ally (ghost={})", id, a_actor->GetGhost());
	}

	// Puts the peer's character name on the puppet (this ref only, not every drifter).
	void ShowPeerName(RE::Actor* a_actor)
	{
		std::string name;
		{
			std::scoped_lock l(g_lock);
			name = g_remoteName;
		}
		if (name.empty() || name == g_shownName || !a_actor->extraList) {
			return;
		}
		a_actor->extraList->SetOverrideName(name.c_str());
		g_shownName = name;
		REX::INFO("Puppet {:08X} named '{}' (shows as '{}')", a_actor->GetFormID(), name, a_actor->GetDisplayFullName());
	}

	// Clones of the player base we created are runtime-only (FFxxxxxx). Any that isn't the
	// live puppet leaked from an earlier session or save, so remove it.
	void RemoveStrayPuppets()
	{
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return;
		}
		const auto live = g_puppet.get();
		for (auto* arr : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
			for (const auto& h : *arr) {
				const auto a = h.get();
				if (!a || a.get() == live.get() || a->IsPlayerRef() || a->IsDisabled() ||
					(a->GetFormID() >> 24) != 0xFF || !a->GetObjectReference() ||
					(a->GetObjectReference()->GetFormID() != kPuppetBase && a->GetObjectReference()->GetFormID() != kPuppetBaseFemale &&
						a->GetObjectReference()->GetFormID() != kOldPuppetBase)) {
					continue;
				}
				if (!g_strayTried.insert(a->GetFormID()).second) {
					continue;  // already tried; don't spam the log every tick
				}
				REX::INFO("Removing stray puppet {:08X}", a->GetFormID());
				a->Disable();
				a->SetDelete(true);
			}
		}
	}

	void SendKill(RE::TESFormID a_id)
	{
		sockaddr_in peer;
		{
			std::scoped_lock l(g_lock);
			if (!g_havePeer) {
				return;
			}
			peer = g_peer;
		}
		const KillPacket k{ kMagic, Type::kKill, a_id };
		for (int i = 0; i < 3; ++i) {  // UDP can drop; applying a kill twice is harmless
			sendto(g_sock, reinterpret_cast<const char*>(&k), sizeof(k), 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
		}
	}

	// Main thread: kill actors the peer reported dead.
	void ApplyKills()
	{
		if (MenuOpen()) {
			return;  // keep them queued until the player is back in gameplay
		}
		std::vector<RE::TESFormID> kills;
		{
			std::scoped_lock l(g_lock);
			kills.swap(g_pendingKills);
		}
		for (const auto id : kills) {
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
			if (!actor) {
				// shortcut: kills of actors not loaded here are dropped; queue them per cell if players split up.
				REX::INFO("Peer killed {:08X} (not loaded here)", id);
			} else if (!actor->IsDead(false) && !g_remoteKills.contains(id)) {  // packets arrive x3
				g_remoteKills.insert(id);
				RE::Console::ExecuteCommand(std::format("{:08X}.kill", id).c_str());
				REX::INFO("Peer killed {:08X} -> killed here (dead now={})", id, actor->IsDead(false));
			}
		}
	}

	float Health(RE::Actor* a_actor)
	{
		const auto av = RE::ActorValue::GetSingleton();
		return av && av->health ? a_actor->GetActorValue(*av->health) : 0.0f;
	}

	void SendTo(const void* a_data, int a_size)
	{
		sockaddr_in peer;
		{
			std::scoped_lock l(g_lock);
			if (!g_havePeer) {
				return;
			}
			peer = g_peer;
		}
		sendto(g_sock, static_cast<const char*>(a_data), a_size, 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
	}

	// Main thread: measure our hits one tick later and send the damage they did.
	void SendHitDamage()
	{
		for (const auto& c : g_hitChecks) {
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(c.actor);
			if (!actor) {
				continue;
			}
			const float now = Health(actor);
			const float measured = c.healthAtHit - now;
			// The event may fire before or after the game applies the damage; the log tells which.
			const float amount = measured > 0.0f ? measured : c.reported;
			REX::INFO("Hit {:08X}: reported {:.1f}, health {:.1f} -> {:.1f} (measured {:.1f}) -> sending {:.1f}",
				c.actor, c.reported, c.healthAtHit, now, measured, amount);
			if (amount > 0.0f && !actor->IsDead(false)) {  // deaths go through the kill message
				const DamagePacket d{ kMagic, Type::kDamage, c.actor, amount };
				SendTo(&d, sizeof(d));
			}
		}
		g_hitChecks.clear();
	}

	// Main thread: apply damage the peer did to actors loaded here.
	void ApplyDamage()
	{
		std::vector<DamagePacket> damage;
		{
			std::scoped_lock l(g_lock);
			damage.swap(g_pendingDamage);
		}
		const auto av = RE::ActorValue::GetSingleton();
		for (const auto& d : damage) {
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(d.actor);
			if (!actor || !av || !av->health || actor->IsDead(false)) {
				continue;
			}
			const float before = Health(actor);
			const float apply = (std::min)(d.amount, before - 1.0f);  // leave the death itself to the kill message
			if (apply > 0.0f) {
				actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *av->health, -apply);
			}
			REX::INFO("Peer hit {:08X} for {:.1f}: health {:.1f} -> {:.1f}", d.actor, d.amount, before, Health(actor));
		}
	}

	// Main thread: share XP both ways. Our gains are found by watching the XP value, so every source
	// (kills, quests, locks, crafting) counts; XP we award from the peer is folded into the baseline.
	void ShareXP(RE::PlayerCharacter* a_player)
	{
		const auto av = RE::ActorValue::GetSingleton();
		if (!av || !av->experience) {
			return;
		}
		const float now = a_player->GetActorValue(*av->experience);
		if (g_xpSeen >= 0.0f && now > g_xpSeen) {
			const float gained = now - g_xpSeen;
			const XPPacket x{ kMagic, Type::kXP, gained };
			SendTo(&x, sizeof(x));
			REX::INFO("Earned {:.0f} XP -> sharing", gained);
		}
		// shortcut: XP earned in the same tick as a level-up (if the value resets) isn't shared; fine for now.
		g_xpSeen = now;

		if (MenuOpen()) {
			return;  // award the peer's XP once no menu is up
		}
		float award;
		{
			std::scoped_lock l(g_lock);
			award = std::exchange(g_pendingXP, 0.0f);
		}
		if (award > 0.0f) {
			a_player->RewardExperience(award, true, nullptr, nullptr);
			g_xpSeen = a_player->GetActorValue(*av->experience);  // don't send it back
			REX::INFO("Peer earned {:.0f} XP -> awarded here (XP now {:.0f})", award, g_xpSeen);
		}
	}

	// Main thread: remember nearby actors' health so a hit can be measured against the value before it.
	void RecordHealth()
	{
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return;
		}
		g_health.clear();
		for (const auto& h : lists->highActorHandles) {
			if (const auto a = h.get(); a && !a->IsPlayerRef() && (a->GetFormID() >> 24) != 0xFF) {
				g_health[a->GetFormID()] = Health(a.get());
			}
		}
	}

	float GraphFloat(RE::Actor* a_actor, const char* a_var)
	{
		float v = 0.0f;
		a_actor->GetGraphVariableImplFloat(a_var, v);
		return v;
	}

	// Dev probe: which locomotion graph variables exist and what they hold, on the player vs the puppet.
	void AnimProbe(RE::PlayerCharacter* a_player, RE::Actor* a_puppet)
	{
		static const char* const kVars[] = { "Speed", "Direction", "SpeedSampled", "SpeedSmoothed", "TurnDelta", "IsMoving",
			"IsSprinting", "bIsSprinting", "bInSprint", "iSyncSprintState", "IsRunning", "bIsRunning", "SpeedWalk", "SpeedRun",
			"SpeedJog", "fSpeedMult", "SpeedMult", "bAnimationDriven", "bMotionDriven", "iState", "iSyncLocomotionSpeed",
			"fLocomotionSpeed", "fMovementSpeed", "bSprintOK", "bIsWalking", "Pitch", "fTimeStep" };
		auto describe = [](RE::Actor* a) {
			std::string out;
			for (const auto v : kVars) {
				float       f = 0;
				bool        b = false;
				std::int32_t i = 0;
				if (a->GetGraphVariableImplFloat(v, f)) {
					out += std::format(" {}={:.2f}", v, f);
				} else if (a->GetGraphVariableImplBool(v, b)) {
					out += std::format(" {}={}", v, b);
				} else if (a->GetGraphVariableImplInt(v, i)) {
					out += std::format(" {}=i{}", v, i);
				}
			}
			return out;
		};
		static auto          last = std::chrono::steady_clock::now();
		static RE::NiPoint3  lastPos = a_player->GetPosition();
		const auto           now = std::chrono::steady_clock::now();
		const float          dt = std::chrono::duration<float>(now - last).count();
		if (dt < 2.0f) {
			return;
		}
		const auto  pos = a_player->GetPosition();
		const float moved = std::sqrt((pos.x - lastPos.x) * (pos.x - lastPos.x) + (pos.y - lastPos.y) * (pos.y - lastPos.y)) / dt;
		last = now;
		lastPos = pos;
		REX::INFO("PROBE you: real speed {:.0f} u/s |{}", moved, describe(a_player));
		if (a_puppet) {
			REX::INFO("PROBE puppet:{}", describe(a_puppet));
			static RE::TESFormID probedEvents = 0;
			if (probedEvents != a_puppet->GetFormID()) {  // once per puppet: which events the graph accepts
				probedEvents = a_puppet->GetFormID();
				std::string ok;
				for (const auto e : { "SprintStart", "SprintStop", "sprintStart", "sprintStop", "moveStart", "moveStop",
						 "runStart", "runStop", "SprintEnter", "SprintExit", "SprintBegin", "SprintEnd", "tSprintStart" }) {
					ok += std::format(" {}={}", e, a_puppet->NotifyAnimationGraphImpl(e));
				}
				REX::INFO("PROBE puppet events accepted:{}", ok);
			}
		}
	}

	std::string ItemName(RE::TESForm* a_form)
	{
		const auto name = RE::TESFullName::GetFullName(*a_form);
		return name.empty() ? std::format("{:08X}", a_form->GetFormID()) : std::string(name);
	}

	// Main thread: send items we dropped next to the puppet, once the dropped copy is gone here.
	// shortcut: only the base item travels (weapon/armor mods and legendary effects are lost); send instance data later.
	void SendGifts()
	{
		for (const auto& d : g_drops) {
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(d.ref);
			const auto item = RE::TESForm::GetFormByID(d.item);
			if (!ref || !item || ref->IsDeleted()) {
				REX::WARN("Give: dropped {:08X} not found; leaving it on the ground", d.ref);
				continue;
			}
			ref->Disable();
			ref->SetDelete(true);
			const GivePacket g{ kMagic, Type::kGive, d.item, d.count };
			SendTo(&g, sizeof(g));
			const auto msg = std::format("Gave {} x{} to {}", ItemName(item), d.count, g_shownName.empty() ? "your friend" : g_shownName);
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, false, false);
			REX::INFO("{}", msg);
		}
		g_drops.clear();
	}

	// Main thread: add items the peer gave us.
	void ReceiveGifts(RE::PlayerCharacter* a_player)
	{
		if (MenuOpen()) {
			return;
		}
		std::vector<GivePacket> gifts;
		{
			std::scoped_lock l(g_lock);
			gifts.swap(g_pendingGifts);
		}
		for (const auto& g : gifts) {
			const auto form = RE::TESForm::GetFormByID(g.item);
			const auto obj = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!obj) {
				REX::WARN("Gift {:08X} isn't an item in this game (different mods?)", g.item);
				continue;
			}
			a_player->AddInventoryItem(obj, nullptr, static_cast<std::uint32_t>(g.count), nullptr, nullptr, nullptr);
			const auto msg = std::format("Received {} x{} from {}", ItemName(form), g.count, g_shownName.empty() ? "your friend" : g_shownName);
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, false, false);
			REX::INFO("{}", msg);
		}
	}

	class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
		{
			// Player (0x14) -> world (0): a drop. Give it if we're standing next to the puppet.
			if (a_event.oldContainerFormID != 0x14 || a_event.newContainerFormID != 0 || a_event.referenceFormID == 0 || a_event.itemCount <= 0 ||
				(a_event.baseObjectFormID >> 24) == 0xFF) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto puppet = g_puppet.get();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!puppet || !player) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto a = player->GetPosition(), b = puppet->GetPosition();
			if ((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) > kGiveRange * kGiveRange) {
				return RE::BSEventNotifyControl::kContinue;
			}
			g_drops.push_back({ a_event.referenceFormID, a_event.baseObjectFormID, a_event.itemCount });
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	ContainerSink g_containerSink;

	// What the local player wears/holds: the object in every biped slot, deduped.
	LookPacket MyLook(RE::PlayerCharacter* a_player)
	{
		LookPacket lk{ kMagic, Type::kLook, static_cast<std::uint8_t>(a_player->GetSex() == RE::SEX::kFemale), 0, {} };
		if (const auto biped = a_player->biped.get()) {
			for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal) && lk.count < kMaxWorn; ++i) {
				const auto obj = biped->object[i].parent.object;
				if (!obj || (obj->GetFormID() >> 24) == 0xFF || (!obj->Is(RE::ENUM_FORM_ID::kARMO) && !obj->Is(RE::ENUM_FORM_ID::kWEAP))) {
					continue;
				}
				if (std::find(lk.worn, lk.worn + lk.count, obj->GetFormID()) == lk.worn + lk.count) {
					lk.worn[lk.count++] = obj->GetFormID();
				}
			}
		}
		return lk;
	}

	// Is the puppet wearing every item in a_want? (reads its biped slots)
	int WornCount(RE::Actor* a_actor, const std::vector<RE::TESFormID>& a_want)
	{
		const auto biped = a_actor->biped.get();
		if (!biped) {
			return 0;
		}
		int n = 0;
		for (const auto id : a_want) {
			for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i) {
				if (const auto o = biped->object[i].parent.object; o && o->GetFormID() == id) {
					++n;
					break;
				}
			}
		}
		return n;
	}

	// Dress the puppet in the peer's items. Console commands run later in the frame, so strip in one
	// tick and dress a moment later, then check the slots and retry (the strip once undressed us).
	// shortcut: base items only, so weapon/armor mods and paint don't show; send instance data later.
	void ApplyLook(RE::Actor* a_actor)
	{
		LookPacket lk;
		{
			std::scoped_lock l(g_lock);
			lk = g_remoteLook;
		}
		if (lk.magic != kMagic || !a_actor->Get3D() || MenuOpen()) {
			return;
		}
		if (g_cfg.puppetBase) {
			return;  // dev base override: leave its look alone (face test)
		}
		const bool wantFemale = lk.female != 0;
		const bool hasFace = g_faceNPC && a_actor->GetObjectReference() == g_faceNPC;  // sex comes with the face
		if (!hasFace && (a_actor->GetSex() == RE::SEX::kFemale) != wantFemale) {
			REX::INFO("Puppet body is the wrong sex; respawning as {}", wantFemale ? "female" : "male");
			DespawnPuppet();  // next tick spawns the right base
			return;
		}
		const std::vector<RE::TESFormID> want(lk.worn, lk.worn + lk.count);
		const auto                       now = std::chrono::steady_clock::now();
		if (want != g_shownLook) {  // new outfit: strip now, dress after the console has run
			g_shownLook = want;
			g_stripped = false;
			g_dressTries = 0;
			RE::Console::ExecuteCommand(std::format("{:08X}.RemoveAllItems", a_actor->GetFormID()).c_str());
			g_stripAt = now;
			return;
		}
		if (g_dressTries >= 3 || now - g_stripAt < std::chrono::milliseconds(300 + 700 * g_dressTries)) {
			return;
		}
		if (g_stripped && WornCount(a_actor, want) == static_cast<int>(want.size())) {
			REX::INFO("Puppet {:08X} fully dressed ({} items)", a_actor->GetFormID(), want.size());
			g_dressTries = 3;  // done: stop checking until the outfit changes
			return;
		}
		// ActorEquipManager::EquipObject silently refused body clothing (Vault 111 Jumpsuit) on the puppet;
		// the console's additem/equipitem work, and run in order after the strip.
		for (const auto id : want) {
			if (WornCount(a_actor, { id }) == 0 && RE::TESForm::GetFormByID(id)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", a_actor->GetFormID(), id).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", a_actor->GetFormID(), id).c_str());
			}
		}
		g_stripped = true;
		++g_dressTries;
		std::string missing;
		for (const auto id : want) {
			if (WornCount(a_actor, { id }) == 0) {
				const auto f = RE::TESForm::GetFormByID(id);
				missing += std::format(" {}({:08X})", f ? ItemName(f) : "?", id);
			}
		}
		REX::INFO("Puppet {:08X} dressing try {}: wearing {}/{} of the peer's items; not worn:{}", a_actor->GetFormID(), g_dressTries,
			WornCount(a_actor, want), want.size(), missing.empty() ? " none" : missing);
	}

	// Main thread: build the peer's look when a new complete blob arrived; respawn the puppet on it.
	void UpdateFace()
	{
		std::uint16_t             version;
		std::vector<std::uint8_t> blob;
		{
			std::scoped_lock l(g_lock);
			if (g_faceReadyVersion == 0 || g_faceReadyVersion == g_faceBuiltVersion) {
				return;
			}
			version = g_faceReadyVersion;
			blob = g_faceReady;
		}
		g_faceBuiltVersion = version;
		if (!g_cfg.realFace) {
			// The engine builds faces live only through the player (faceNPC link): copies either share the
			// player's face (and swap it) or have no head. Off until we can regenerate an NPC head ourselves.
			return;
		}
		// Saved before building, so a crash while building/spawning can be replayed solo (Debug FaceFile).
		std::ofstream("Data/F4SE/Plugins/F4Online-peer-look.bin", std::ios::binary)
			.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
		if (const auto npc = Appearance::BuildNPC(blob, g_cfg.faceMode == 3 ? 0 : g_cfg.faceMode)) {
			g_faceNPC = npc;
			REX::INFO("Peer's look {:04X} ready ({} bytes); respawning the puppet with it", version, blob.size());
			DespawnPuppet();
		}
	}

	void SendFace(RE::PlayerCharacter* a_player, const sockaddr_in& a_peer)
	{
		const auto npc = a_player->GetNPC();
		if (!npc) {
			return;
		}
		auto blob = Appearance::Serialize(npc, a_player->GetSex() == RE::SEX::kFemale);
		if (g_cfg.mode == Mode::kEcho && !g_cfg.faceFile.empty()) {
			std::ifstream f(g_cfg.faceFile, std::ios::binary);
			blob.assign(std::istreambuf_iterator<char>(f), {});
		}
		std::uint16_t version = 0;
		for (const auto b : blob) {
			version = static_cast<std::uint16_t>(version * 31 + b);
		}
		version = version ? version : 1;  // 0 means "none"
		const auto total = static_cast<std::uint8_t>((blob.size() + kFaceChunk - 1) / kFaceChunk);
		if (total == 0 || total > kMaxFaceChunks) {
			return;
		}
		for (std::uint8_t i = 0; i < total; ++i) {
			FacePacket fc{ kMagic, Type::kFace, version, i, total, 0, {} };
			const std::size_t off = i * kFaceChunk;
			fc.len = static_cast<std::uint16_t>((std::min)(kFaceChunk, blob.size() - off));
			std::memcpy(fc.data, blob.data() + off, fc.len);
			sendto(g_sock, reinterpret_cast<const char*>(&fc), static_cast<int>(offsetof(FacePacket, data) + fc.len), 0,
				reinterpret_cast<const sockaddr*>(&a_peer), sizeof(a_peer));
		}
	}

	// ---- shared building ------------------------------------------------------------------
	// shortcut: live only. Mirrors persist in the receiver's save, but the buildID map doesn't, so moves/scraps
	// of objects from an earlier session aren't mirrored. Mirrors aren't linked to the receiver's workshop
	// (no resources, power or settler use). Upgrade: save the map with F4SE serialization, link via WorkshopItemKeyword.
	BuildPacket MakeBuild(BuildOp a_op, std::uint32_t a_id, RE::TESObjectREFR* a_ref)
	{
		BuildPacket b{ kMagic, Type::kBuild, a_op, a_id, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		if (a_ref) {
			const auto cell = a_ref->GetParentCell();
			const auto pos = a_ref->GetPosition();
			b.base = a_ref->GetObjectReference() ? a_ref->GetObjectReference()->GetFormID() : 0;
			b.x = pos.x, b.y = pos.y, b.z = pos.z;
			b.ax = a_ref->data.angle.x, b.ay = a_ref->data.angle.y, b.az = a_ref->data.angle.z;
			b.interior = cell && cell->IsInterior();
			b.area = !cell ? 0 : cell->IsInterior() ? cell->GetFormID() : cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
		}
		return b;
	}

	class PlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
		{
			const auto ref = a_event.placedItem.get();
			if (!ref || !ref->GetObjectReference() || (ref->GetObjectReference()->GetFormID() >> 24) == 0xFF) {
				return RE::BSEventNotifyControl::kContinue;
			}
			static const std::uint32_t salt = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) << 16;
			const auto id = salt | (++g_buildCounter & 0xFFFF);
			g_myBuilds[ref->GetFormID()] = id;
			const auto b = MakeBuild(BuildOp::kPlace, id, ref);
			SendTo(&b, sizeof(b));
			REX::INFO("Built {:08X} (base {:08X}) -> sent as build {:08X}", ref->GetFormID(), b.base, id);
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	PlacedSink g_placedSink;

	class MovedSink : public RE::BSTEventSink<RE::Workshop::ItemMovedEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemMovedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemMovedEvent>*) override
		{
			const auto ref = a_event.movedItem.get();
			const auto it = ref ? g_myBuilds.find(ref->GetFormID()) : g_myBuilds.end();
			if (it != g_myBuilds.end()) {
				const auto b = MakeBuild(BuildOp::kMove, it->second, ref);
				SendTo(&b, sizeof(b));
				REX::INFO("Moved build {:08X}", it->second);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	MovedSink g_movedSink;

	// Main thread, ~1 s: our builds that were scrapped, stored or destroyed.
	void CheckMyBuilds()
	{
		for (auto it = g_myBuilds.begin(); it != g_myBuilds.end();) {
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(it->first);
			if (!ref || ref->IsDeleted() || ref->IsDisabled()) {
				const auto b = MakeBuild(BuildOp::kRemove, it->second, nullptr);
				SendTo(&b, sizeof(b));
				REX::INFO("Build {:08X} is gone here -> removing it for the peer", it->second);
				it = g_myBuilds.erase(it);
			} else {
				++it;
			}
		}
	}

	void RemoveMirror(std::uint32_t a_id)
	{
		if (const auto it = g_mirrors.find(a_id); it != g_mirrors.end()) {
			if (const auto ref = it->second.get()) {
				ref->Disable();
				ref->SetDelete(true);
			}
			g_mirrors.erase(it);
		}
	}

	// Main thread: create/move/remove copies of the peer's builds.
	void ApplyBuilds()
	{
		if (MenuOpen()) {  // includes the workshop menu: wait until we're back in gameplay
			return;
		}
		std::vector<BuildPacket> builds;
		{
			std::scoped_lock l(g_lock);
			builds.swap(g_pendingBuilds);
		}
		for (const auto& b : builds) {
			RemoveMirror(b.buildID);  // a move is remove + place
			if (b.op == BuildOp::kRemove) {
				REX::INFO("Peer removed build {:08X}", b.buildID);
				continue;
			}
			const auto         form = RE::TESForm::GetFormByID(b.base);
			const auto         obj = form ? form->As<RE::TESBoundObject>() : nullptr;
			RE::TESObjectCELL* cell = b.interior ? RE::TESForm::GetFormByID<RE::TESObjectCELL>(b.area) : nullptr;
			RE::TESWorldSpace* world = b.interior ? nullptr : RE::TESForm::GetFormByID<RE::TESWorldSpace>(b.area);
			if (!obj || (!cell && !world)) {
				REX::WARN("Peer's build {:08X}: base {:08X} or area {:08X} not found here", b.buildID, b.base, b.area);
				continue;
			}
			RE::NEW_REFR_DATA d;
			d.location = { b.x + (g_cfg.mode == Mode::kEcho ? kEchoOffset : 0.0f), b.y, b.z };
			d.direction = { b.ax, b.ay, b.az };
			d.object = obj;
			d.interior = cell;
			d.world = world;
			d.clearStillLoadingFlag = true;
			const auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(d);
			if (const auto ref = handle.get()) {
				g_mirrors[b.buildID] = handle;
				REX::INFO("Peer {} build {:08X}: {} at ({:.0f}, {:.0f}, {:.0f}) -> {:08X}", b.op == BuildOp::kMove ? "moved" : "placed",
					b.buildID, ItemName(form), b.x, b.y, b.z, ref->GetFormID());
			}
		}
	}

	class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			const auto cause = a_event.cause.get();
			const auto target = a_event.target.get();
			const auto actor = target ? target->As<RE::Actor>() : nullptr;
			if (!cause || !cause->IsPlayerRef() || !actor || actor->IsPlayerRef() || (actor->GetFormID() >> 24) == 0xFF || actor->IsDead(false)) {
				return RE::BSEventNotifyControl::kContinue;  // only our hits on world-placed, living actors
			}
			// The event fires after the damage is applied, so "before" comes from last tick's record.
			const auto it = g_health.find(actor->GetFormID());
			const float before = it != g_health.end() ? it->second : Health(actor);
			g_hitChecks.push_back({ actor->GetFormID(), before, a_event.usesHitData ? a_event.hitData.healthDamage : 0.0f });
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	HitSink g_hitSink;

	class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
		{
			const auto dying = a_event.actorDying.get();
			if (!a_event.dead || !dying || dying->IsPlayerRef() || (dying->GetFormID() >> 24) == 0xFF) {
				return RE::BSEventNotifyControl::kContinue;  // only finished deaths of world-placed actors
			}
			const auto id = dying->GetFormID();
			if (g_remoteKills.erase(id)) {
				return RE::BSEventNotifyControl::kContinue;  // the peer killed it; don't bounce it back
			}
			REX::INFO("{:08X} died here -> telling peer", id);
			SendKill(id);
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	DeathSink g_deathSink;

	// Main thread only (queued through F4SE's task interface).
	void Tick()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		const auto tickDone = [] {
			g_step = "idle";
			g_tickDoneNs = std::chrono::steady_clock::now().time_since_epoch().count();
		};
		if (!cell || (cell->IsExterior() && !cell->worldSpace)) {
			tickDone();
			return;  // main menu or loading
		}
		const bool interior = cell->IsInterior();
		const auto area = interior ? cell->GetFormID() : cell->worldSpace->GetFormID();
		g_step = "RemoveStrayPuppets";
		RemoveStrayPuppets();
		g_step = "ApplyKills";
		ApplyKills();
		g_step = "SendHitDamage";
		SendHitDamage();
		g_step = "ApplyDamage";
		ApplyDamage();
		g_step = "RecordHealth";
		RecordHealth();
		g_step = "ShareXP";
		ShareXP(player);
		static bool selfTested = false;
		if (!selfTested && player->GetNPC()) {
			selfTested = true;
			Appearance::SelfTest(player->GetNPC(), player->GetSex() == RE::SEX::kFemale);
		}
		g_step = "UpdateFace";
		UpdateFace();
		g_step = "SendGifts";
		SendGifts();
		g_step = "ReceiveGifts";
		ReceiveGifts(player);
		g_step = "ApplyBuilds";
		ApplyBuilds();
		static int buildCheck = 0;
		if (++buildCheck % 60 == 0) {  // ~1 s
			CheckMyBuilds();
		}
		if (g_cfg.animProbe) {
			AnimProbe(player, g_puppet.get() ? g_puppet.get()->As<RE::Actor>() : nullptr);
		}

		g_step = "SendState";
		// Send our state (every kSendEvery ticks).
		static int tickNo = 0;
		const bool sendNow = ++tickNo % kSendEvery == 0;
		const auto pos = player->GetPosition();
		bool       sprinting = false;
		player->GetGraphVariableImplBool("IsSprinting", sprinting);
		Packet out{ kMagic, Type::kState, ++g_seq, pos.x, pos.y, pos.z, player->data.angle.z, area, static_cast<std::uint8_t>(interior),
			GraphFloat(player, "Speed"), GraphFloat(player, "Direction"), static_cast<std::uint8_t>(sprinting),
			static_cast<std::uint8_t>(player->GetWeaponMagicDrawn()) };
		sockaddr_in peer{};
		bool        havePeer;
		RemoteState remote;
		{
			std::scoped_lock l(g_lock);
			peer = g_peer;
			havePeer = g_havePeer;
			remote = g_remote;
		}
		if (havePeer && sendNow) {
			sendto(g_sock, reinterpret_cast<const char*>(&out), sizeof(out), 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
		}
		if (havePeer && tickNo % 120 == 0) {  // ~2 s: our name, so late joiners and lost packets catch up
			NamePacket nm{ kMagic, Type::kName, {} };
			if (const auto name = player->GetDisplayFullName()) {
				strncpy_s(nm.name, name, _TRUNCATE);
			}
			sendto(g_sock, reinterpret_cast<const char*>(&nm), sizeof(nm), 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
			const auto look = MyLook(player);
			sendto(g_sock, reinterpret_cast<const char*>(&look), sizeof(look), 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
		}
		if (havePeer && tickNo % 180 == 60) {  // ~3 s, offset from the name/look burst
			SendFace(player, peer);
		}

		g_step = "Puppet";
		// Drive the puppet from the latest remote state.
		const bool sameArea = remote.valid && remote.pkt.area == area && remote.pkt.interior == static_cast<std::uint8_t>(interior);
		if (!sameArea || std::chrono::steady_clock::now() - remote.at > kStale) {
			DespawnPuppet();
			tickDone();
			return;
		}
		auto p = remote.pkt;
		if (g_cfg.mode == Mode::kEcho) {
			p.x += kEchoOffset;
		}
		auto actor = g_puppet.get() ? g_puppet.get()->As<RE::Actor>() : SpawnPuppet(p, cell);
		if (!actor) {
			tickDone();
			return;
		}
		MakePuppetPeaceful(actor);
		ShowPeerName(actor);
		if (g_faceSwapped) {  // give the face fields back once the puppet's head exists
			bool hasHead = false;
			if (const auto root = actor->Get3D()) {
				hasHead = root->GetObjectByName("BSFaceGenNiNodeSkinned") != nullptr;
			}
			if (hasHead || std::chrono::steady_clock::now() - g_spawnedAt > std::chrono::seconds(5)) {
				Appearance::SwapFace(RE::TESForm::GetFormByID<RE::TESNPC>(0x7), g_faceNPC);
				g_faceSwapped = false;
				REX::INFO("FaceMode 3: face fields returned to the player base (puppet head built: {})", hasHead);
			}
		}
		static RE::TESFormID dumped = 0;
		if (g_cfg.realFace && dumped != actor->GetFormID() && std::chrono::steady_clock::now() - g_spawnedAt > std::chrono::seconds(3)) {
			dumped = actor->GetFormID();
			Appearance::DumpHead(actor);
			Appearance::DumpHead(player);
		}
		ApplyLook(actor);
		if (!g_puppet.get()) {
			tickDone();
			return;  // ApplyLook respawns the puppet when the sex is wrong
		}

		// Predict where they are now from their velocity (packets are ~50 ms apart), then glide there.
		// This keeps a steady pace at run/sprint speed instead of catch-up-and-slow-down.
		const float        ahead = (std::min)(std::chrono::duration<float>(std::chrono::steady_clock::now() - remote.at).count(), 0.25f);
		const RE::NiPoint3 target{ p.x + remote.vx * ahead, p.y + remote.vy * ahead, p.z + remote.vz * ahead };
		const float        gap = std::sqrt((target.x - g_shownPos.x) * (target.x - g_shownPos.x) + (target.y - g_shownPos.y) * (target.y - g_shownPos.y));
		if (!g_shownValid || gap > 800.0f) {  // first frame or a teleport/fast travel: snap
			g_shownPos = target;
			g_shownYaw = p.yaw;
			g_shownValid = true;
		} else {
			constexpr float k = 0.35f;  // per 16 ms tick; the prediction already carries the motion
			g_shownPos.x += (target.x - g_shownPos.x) * k;
			g_shownPos.y += (target.y - g_shownPos.y) * k;
			g_shownPos.z += (target.z - g_shownPos.z) * k;
			float dyaw = std::remainder(p.yaw - g_shownYaw, 6.2831853f);  // shortest way round
			g_shownYaw += dyaw * k;
		}
		actor->SetPosition(g_shownPos, true);
		actor->SetAngleOnReference({ 0.0f, 0.0f, g_shownYaw });
		// Drive the locomotion blend so it walks/runs instead of sliding.
		actor->SetGraphVariableFloat("Speed", p.speed);
		actor->SetGraphVariableFloat("Direction", p.dir);
		actor->SetGraphVariableBool("IsSprinting", p.sprint != 0);
		// Weapon out/away like the peer (NPCs don't show holstered guns). Act once per change: re-drawing
		// every second made the calm AI put it away again and again (a draw-animation loop). SetAlert is the
		// game's "keep your weapon ready" state for NPCs.
		static bool wantDrawn = false;
		static auto changedAt = std::chrono::steady_clock::time_point{};
		static bool checked = true;
		// Wait for menus to close: the console can't run in them, and a skipped SetAlert 0 left the gun out (workshop holsters you).
		if ((p.drawn != 0) != wantDrawn && actor->Get3D() && !MenuOpen() && std::chrono::steady_clock::now() - g_spawnedAt > std::chrono::seconds(1)) {
			wantDrawn = p.drawn != 0;
			changedAt = std::chrono::steady_clock::now();
			checked = false;
			RE::Console::ExecuteCommand(std::format("{:08X}.SetAlert {}", actor->GetFormID(), wantDrawn ? 1 : 0).c_str());
			actor->DrawWeaponMagicHands(wantDrawn);
			REX::INFO("Puppet {:08X} {} weapon", actor->GetFormID(), wantDrawn ? "draws" : "holsters");
		} else if (!checked && std::chrono::steady_clock::now() - changedAt > std::chrono::seconds(1)) {
			checked = true;
			REX::INFO("Puppet {:08X} weapon {} after 1 s (wanted {})", actor->GetFormID(),
				actor->GetWeaponMagicDrawn() ? "out" : "away", wantDrawn ? "out" : "away");
		}
		// NPCs enter/leave the sprint animation on graph events, not just the variable.
		static bool puppetSprinting = false;
		if ((p.sprint != 0) != puppetSprinting) {
			puppetSprinting = p.sprint != 0;
			const bool ok = actor->NotifyAnimationGraphImpl(puppetSprinting ? "sprintStart" : "sprintStop");
			REX::INFO("Puppet {} sprint (graph accepted event: {})", puppetSprinting ? "starts" : "stops", ok);
		}

		static auto lastLog = std::chrono::steady_clock::now();
		if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5)) {
			lastLog = std::chrono::steady_clock::now();
			const auto  at = actor->GetPosition();  // where the puppet really is, not where we asked
			const float dx = at.x - pos.x, dy = at.y - pos.y;
			REX::INFO("Puppet {:08X}: target ({:.0f}, {:.0f}, {:.0f}) actual ({:.0f}, {:.0f}, {:.0f}), {:.0f} units from you, 3D={} cell={:08X} | anim sent speed={:.0f} dir={:.2f} sprint={} / puppet graph speed={:.0f} dir={:.2f}",
				actor->GetFormID(), p.x, p.y, p.z, at.x, at.y, at.z, std::sqrt(dx * dx + dy * dy),
				actor->Get3D() != nullptr, actor->GetParentCell() ? actor->GetParentCell()->GetFormID() : 0,
				p.speed, p.dir, p.sprint, GraphFloat(actor, "Speed"), GraphFloat(actor, "Direction"));
		}
		tickDone();
	}

	bool OpenSocket()
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			REX::ERROR("WSAStartup failed");
			return false;
		}
		g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (g_sock == INVALID_SOCKET) {
			REX::ERROR("socket() failed: {}", WSAGetLastError());
			return false;
		}
		u_long nonBlocking = 1;
		ioctlsocket(g_sock, FIONBIO, &nonBlocking);

		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = htons(g_cfg.mode == Mode::kJoin ? 0 : g_cfg.port);  // guests use any free port
		if (bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
			REX::ERROR("bind to UDP {} failed: {} (is another program using it?)", g_cfg.port, WSAGetLastError());
			return false;
		}

		if (g_cfg.mode != Mode::kHost) {  // join and echo know their peer up front
			g_peer.sin_family = AF_INET;
			g_peer.sin_port = htons(g_cfg.port);
			if (inet_pton(AF_INET, g_cfg.mode == Mode::kEcho ? "127.0.0.1" : g_cfg.address.c_str(), &g_peer.sin_addr) != 1) {
				REX::ERROR("Bad Address '{}' in F4Online.ini (use an IP like 203.0.113.5)", g_cfg.address);
				return false;
			}
			g_havePeer = true;
		}
		return true;
	}
}

namespace Sync
{
	void UnswapFace()
	{
		if (g_faceSwapped) {  // never let the player's save or a load see the peer's face fields
			Appearance::SwapFace(RE::TESForm::GetFormByID<RE::TESNPC>(0x7), g_faceNPC);
			g_faceSwapped = false;
		}
	}

	const char* CurrentStep()
	{
		return g_step.load();
	}

	void OnPreSave()
	{
		UnswapFace();
		g_noSwapUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		DespawnPuppet();
	}

	void OnPreLoad()
	{
		UnswapFace();
		g_puppet.reset();
		g_shownValid = false;
		g_shownName.clear();
		g_faceNPC = nullptr;        // runtime-created forms don't survive a load; rebuild from the blob
		g_faceBuiltVersion = 0;
		g_xpSeen = -1.0f;  // a loaded save's XP isn't a gain
	}

	void Start(const Config& a_cfg)
	{
		g_cfg = a_cfg;
		if (g_cfg.mode == Mode::kOff || !OpenSocket()) {
			return;
		}
		switch (g_cfg.mode) {
		case Mode::kHost: REX::INFO("Hosting on UDP {}", g_cfg.port); break;
		case Mode::kJoin: REX::INFO("Joining {}", Addr(g_peer)); break;
		default:          REX::INFO("Echo test on UDP {} (puppet copies you)", g_cfg.port); break;
		}
		if (const auto deaths = RE::TESDeathEvent::GetEventSource()) {
			deaths->RegisterSink(&g_deathSink);
		} else {
			REX::ERROR("No death event source: shared kills disabled");
		}
		if (g_cfg.building) {
			// Diagnostics first: where these IDs resolve and the code there (a wrong ID froze the game once).
			const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
			for (const auto& [name, id] : { std::pair{ "RegisterForItemPlaced", RE::ID::Workshop::RegisterForItemPlaced },
					 std::pair{ "RegisterForItemMoved", RE::ID::Workshop::RegisterForItemMoved } }) {
				const auto addr = id.address();
				std::string bytes;
				for (int i = 0; i < 48; ++i) {
					bytes += std::format("{:02X}", reinterpret_cast<const std::uint8_t*>(addr)[i]);
				}
				REX::INFO("Building: {} = ID {} -> +{:X}: {}", name, id.id(), addr - base, bytes);
			}
			RE::Workshop::RegisterForItemPlaced(&g_placedSink);
			REX::INFO("Building: subscribed to placed");
			RE::Workshop::RegisterForItemMoved(&g_movedSink);
			REX::INFO("Building: subscribed to moved");
		}
		if (const auto moves = RE::TESContainerChangedEvent::GetEventSource()) {
			moves->RegisterSink(&g_containerSink);
		} else {
			REX::ERROR("No container event source: giving items disabled");
		}
		if (const auto hits = RE::TESHitEvent::GetEventSource()) {
			hits->RegisterSink(&g_hitSink);
		} else {
			REX::ERROR("No hit event source: shared damage disabled");
		}
		g_thread = std::jthread([](std::stop_token a_stop) {
			if (!g_cfg.testStart.empty()) {
				std::this_thread::sleep_for(std::chrono::seconds(8));  // let the main menu settle
				F4SE::GetTaskInterface()->AddTask([] {
					REX::INFO("TestStart: coc {}", g_cfg.testStart);
					RE::Console::ExecuteCommand(std::format("coc {}", g_cfg.testStart).c_str());
				});
			}
			bool stalled = false;
			while (!a_stop.stop_requested()) {
				const auto doneNs = g_tickDoneNs.load();
				const auto nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
				const auto since = doneNs ? std::chrono::nanoseconds(nowNs - doneNs) : std::chrono::nanoseconds(0);
				if (doneNs && since > std::chrono::seconds(3) && !stalled) {
					stalled = true;
					REX::WARN("WATCHDOG: game main thread hasn't finished an F4 Online tick for {:.1f} s; F4 Online step: {}",
						std::chrono::duration<float>(since).count(), g_step.load());
				} else if (stalled && since < std::chrono::seconds(1)) {
					stalled = false;
					REX::INFO("WATCHDOG: main thread running again");
				}
				ReceiveAll();
				F4SE::GetTaskInterface()->AddTask(Tick);
				std::this_thread::sleep_for(kTick);
			}
		});
	}
}
