#include "sync.h"

#include "crashlog.h"

#include <Windows.h>

namespace
{
	Config LoadConfig()
	{
		// Data\F4SE\Plugins\F4Online.ini; the game's working directory is its install folder.
		const auto path = std::filesystem::absolute("Data\\F4SE\\Plugins\\F4Online.ini").string();
		char       buf[256]{};
		Config     cfg;

		GetPrivateProfileStringA("Network", "Mode", "off", buf, sizeof(buf), path.c_str());
		const std::string mode = buf;
		cfg.mode = mode == "host" ? Mode::kHost : mode == "join" ? Mode::kJoin : mode == "echo" ? Mode::kEcho : Mode::kOff;

		GetPrivateProfileStringA("Network", "Address", "", buf, sizeof(buf), path.c_str());
		cfg.address = buf;
		const auto port = GetPrivateProfileIntA("Network", "Port", 27780, path.c_str());
		if (port > 0 && port < 65536) {
			cfg.port = static_cast<std::uint16_t>(port);
		}
		GetPrivateProfileStringA("Debug", "TestStart", "", buf, sizeof(buf), path.c_str());
		cfg.testStart = buf;
		cfg.animProbe = GetPrivateProfileIntA("Debug", "AnimProbe", 0, path.c_str()) != 0;
		cfg.realFace = GetPrivateProfileIntA("Debug", "RealFace", 1, path.c_str()) != 0;
		cfg.faceMode = GetPrivateProfileIntA("Debug", "FaceMode", 3, path.c_str());
		cfg.building = GetPrivateProfileIntA("Debug", "Building", 0, path.c_str()) != 0;
		GetPrivateProfileStringA("Debug", "FaceFile", "", buf, sizeof(buf), path.c_str());
		cfg.faceFile = buf;
		GetPrivateProfileStringA("Debug", "PuppetBase", "", buf, sizeof(buf), path.c_str());
		cfg.puppetBase = std::string(buf) == "dup" ? kPuppetBaseDupPlayer : static_cast<std::uint32_t>(std::strtoul(buf, nullptr, 16));
		REX::INFO("Config: Mode={} Address={} Port={} TestStart={}", mode, cfg.address, cfg.port, cfg.testStart);
		return cfg;
	}

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			if (GetPrivateProfileIntA("Debug", "CrashLogTest", 0, std::filesystem::absolute("Data/F4SE/Plugins/F4Online.ini").string().c_str())) {
				CrashLog::SelfTest();
			}
			Sync::Start(LoadConfig());
			break;
		case F4SE::MessagingInterface::kPreSaveGame:   Sync::OnPreSave(); break;
		case F4SE::MessagingInterface::kPreLoadGame:   Sync::OnPreLoad(); break;
		default:                                       break;
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	REX::INFO("F4 Online loaded");
	CrashLog::Install();
	F4SE::GetMessagingInterface()->RegisterListener(OnMessage);

	return true;
}
