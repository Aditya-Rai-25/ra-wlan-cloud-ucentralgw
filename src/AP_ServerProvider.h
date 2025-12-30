//
// Created by Codex automation
//

#pragma once

#include <atomic>

#include "AP_SERVER.h"

namespace OpenWifi {

	class AP_ServerProvider {
	  public:
		static void Register(AP_Server *server);
		[[nodiscard]] static AP_Server *Get();

	  private:
		AP_ServerProvider() = default;
	};

	inline AP_Server *GetAPServer() { return AP_ServerProvider::Get(); }

} // namespace OpenWifi

