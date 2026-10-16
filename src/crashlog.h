#pragma once

// Logs fatal exceptions (code, module+offset, unwound stack, F4 Online step): Fallout 4 leaves no Windows crash record.
namespace CrashLog
{
	void Install();
	void SelfTest();  // dev: [Debug] CrashLogTest=1 - caught null read, should log CRASH lines
}
