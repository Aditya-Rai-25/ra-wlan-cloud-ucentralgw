//
// Created by Codex automation
//

#include "AP_ServerProvider.h"

#include <stdexcept>

namespace OpenWifi {
	namespace {
		std::atomic<AP_Server *> CurrentServer{nullptr};
	}

	void AP_ServerProvider::Register(AP_Server *server) { CurrentServer.store(server); }

	AP_Server *AP_ServerProvider::Get() {
		auto *server = CurrentServer.load();
		if (server == nullptr) {
			throw std::runtime_error("AP server provider not registered");
		}
		return server;
	}
} // namespace OpenWifi

