#pragma once

namespace game
{
	class console
	{
	public:
		static void printf(const char* fmt, ...) {
			static auto p1 = RelocAddr<std::uintptr_t*>(0x061E7540); // 0x060C12D8
			static auto p2 = RelocAddr<std::uintptr_t(*)(std::uintptr_t, const char*, va_list)>(0x01EAD440); // 0x01E358B0

			if (*p1 && p2) {
				auto args = va_list();
				va_start(args, fmt);
				(*p2)(*p1, fmt, args);
				va_end(args);
			}
		}

		static void execute(const std::string& cmd) {
			static auto p1 = RelocAddr<std::uintptr_t*>(0x062C8A58); // 0x061A2110
			static auto p2 = RelocAddr<void(*)(std::uintptr_t, const char*)>(0x01EA48D0); // 0x01E2CD70
			(*p2)(*p1, cmd.data());
		}
	};
}
