#include "crashlog.h"

#include "sync.h"

#include <Windows.h>
#undef ERROR  // wingdi.h macro clashes with REX::ERROR

namespace
{
	std::string Where(std::uintptr_t a_addr)
	{
		HMODULE mod = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_addr), &mod) || !mod) {
			return std::format("{:X} (no module)", a_addr);
		}
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(mod, path, MAX_PATH);
		const auto name = std::filesystem::path(path).filename().string();
		return std::format("{}+{:X}", name, a_addr - reinterpret_cast<std::uintptr_t>(mod));
	}

	std::atomic<int> g_reports{ 0 };

	LONG CALLBACK OnException(EXCEPTION_POINTERS* a_info)
	{
		const auto code = a_info->ExceptionRecord->ExceptionCode;
		const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
		                   code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
		if (!fatal || g_reports.fetch_add(1) >= 3) {  // first-chance: the game may handle some itself; cap the noise
			return EXCEPTION_CONTINUE_SEARCH;
		}
		const auto pc = reinterpret_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionAddress);
		std::string detail;
		if (code == EXCEPTION_ACCESS_VIOLATION && a_info->ExceptionRecord->NumberParameters >= 2) {
			detail = std::format(" ({} {:X})", a_info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
				a_info->ExceptionRecord->ExceptionInformation[1]);
		}
		REX::ERROR("CRASH {:08X}{} at {} on thread {}; F4 Online step: {}", code, detail, Where(pc), GetCurrentThreadId(), Sync::CurrentStep());

		// Unwind from the faulting context (x64 unwind tables).
		CONTEXT ctx = *a_info->ContextRecord;
		std::string stack;
		for (int i = 0; i < 24 && ctx.Rip; ++i) {
			stack += std::format("\n    #{:02} {}", i, Where(ctx.Rip));
			DWORD64 imageBase = 0;
			const auto fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
			if (!fn) {  // leaf function: return address is on top of the stack
				ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
				ctx.Rsp += 8;
				continue;
			}
			void*   handlerData = nullptr;
			DWORD64 establisher = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
		}
		REX::ERROR("CRASH stack:{}", stack);
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

namespace CrashLog
{
	void Install()
	{
		AddVectoredExceptionHandler(1, OnException);
		REX::INFO("Crash logger installed");
	}

	void SelfTest()
	{
		// A caught null read: the handler logs it (first chance), then __except swallows it.
		__try {
			volatile int* p = nullptr;
			[[maybe_unused]] volatile int v = *p;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::INFO("Crash logger self-test: caught the test exception (the CRASH lines above are expected)");
		}
		g_reports = 0;  // the test doesn't count against the real-crash cap
	}
}
