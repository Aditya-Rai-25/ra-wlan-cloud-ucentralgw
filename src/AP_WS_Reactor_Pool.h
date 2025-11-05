/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Created by stephane bourque on 2022-02-03.
//

#pragma once

#include <mutex>
#include <string>

#include <framework/utils.h>

#include <Poco/Environment.h>
#include <Poco/Net/SocketAcceptor.h>
#include <Poco/Data/SessionPool.h>

#include <StorageService.h>

namespace OpenWifi {

	class AP_WS_ReactorThreadPool {
	  public:
		explicit AP_WS_ReactorThreadPool(Poco::Logger &Logger) : Logger_(Logger) {
			NumberOfThreads_ = Poco::Environment::processorCount() * 4;
			if (NumberOfThreads_ == 0)
				NumberOfThreads_ = 8;
			NumberOfThreads_ = std::min(NumberOfThreads_, (std::uint64_t) 128);
		}

		~AP_WS_ReactorThreadPool() { Stop(); }

		void Start() {
			Reactors_.reserve(NumberOfThreads_);
			DbSessions_.reserve(NumberOfThreads_);
			Threads_.reserve(NumberOfThreads_);
			Logger_.information(fmt::format("WebSocket Processor: starting {} threads.", NumberOfThreads_));
			for (uint64_t i = 0; i < NumberOfThreads_; ++i) {
				auto NewReactor = std::make_shared<Poco::Net::SocketReactor>();
				auto NewThread = std::make_unique<Poco::Thread>();
				NewThread->start(*NewReactor);
				std::string ThreadName{"ap:react:" + std::to_string(i)};
				Utils::SetThreadName(*NewThread, ThreadName.c_str());
				Reactors_.emplace_back(std::move(NewReactor));
				Threads_.emplace_back(std::move(NewThread));
				DbSessions_.emplace_back(std::make_shared<LockedDbSession>());
			}
			Logger_.information(fmt::format("WebSocket Processor: {} threads started.", NumberOfThreads_));
		}

		void Stop() {
			for (auto &i : Reactors_)
				i->stop();
			for (auto &i : Threads_) {
				i->join();
			}
			Reactors_.clear();
			Threads_.clear();
			DbSessions_.clear();
		}

		auto NextReactor() {
			std::lock_guard Lock(Mutex_);
			NextReactor_++;
			NextReactor_ %= NumberOfThreads_;
			return std::make_pair(Reactors_[NextReactor_], DbSessions_[NextReactor_]);
		}
		/**
		 * @brief Retrieve the next pooled database session in round-robin order.
		 *
		 * The reactor pool pre-allocates one `LockedDbSession` per worker thread during
		 * `Start()`. Each call here increments a shared index under the pool mutex,
		 * wraps it modulo `NumberOfThreads_`, and returns the corresponding entry in
		 * `DbSessions_`.  When the pool is uninitialised (`NumberOfThreads_ == 0`)
		 * the method returns an empty pointer so callers can fall back to constructing
		 * their own session.
		 *
		 * @return Shared pointer to the selected `LockedDbSession`
		 */
		std::shared_ptr<LockedDbSession> NextDbSession() {
			std::lock_guard Lock(Mutex_);
			NextDbSession_++;
			if (NumberOfThreads_ == 0) {
				return {};
			}
			NextDbSession_ %= NumberOfThreads_;
			return DbSessions_[NextDbSession_];
		}

	  private:
		std::mutex Mutex_;
		uint64_t NumberOfThreads_;
		uint64_t NextReactor_ = 0;
		uint64_t NextDbSession_ = 0;
		std::vector<std::shared_ptr<Poco::Net::SocketReactor>> 	Reactors_;
		std::vector<std::unique_ptr<Poco::Thread>> 				Threads_;
		std::vector<std::shared_ptr<LockedDbSession>>			DbSessions_;
		Poco::Logger &Logger_;

	};
} // namespace OpenWifi