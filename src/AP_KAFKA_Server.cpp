/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#include "AP_KAFKA_Server.h"

#include <cctype>
#include <sstream>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/String.h>
#include <Poco/Thread.h>

#include <fmt/format.h>

#include "AP_KAFKA_Connection.h"
#include "AP_ServerProvider.h"
#include "StorageService.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"

namespace OpenWifi {

	bool AP_KAFKA_Server::ValidateCertificate([[maybe_unused]] const std::string &ConnectionId,
											 [[maybe_unused]] const Poco::Crypto::X509Certificate
												 &Certificate) {
		return true;
	}

	int AP_KAFKA_Server::Start() {

		if (!KafkaManager()->Enabled()) {
			poco_warning(Logger(), "Kafka server enabled but KafkaManager is disabled (openwifi.kafka.enable=false).");
			 std::exit(Poco::Util::Application::EXIT_CONFIG);
		}

		if (WatcherId_ == 0) {
			Types::TopicNotifyFunction F = [this](const std::string &Key,
												 const std::string &Payload) {
				this->OnKafkaMessage(Key, Payload);
			};
			WatcherId_ = KafkaManager()->RegisterTopicWatcher(KafkaTopics::CNC_RES, F);
		}
		GarbageCollectorName="KFK-Session-Janitor";
		ReadEnvironment();
		SetJanitor("kafka:garbage");
		poco_information(Logger(), fmt::format("Kafka server started."));
		return 0;
	}

	void AP_KAFKA_Server::Stop() {

		poco_information(Logger(), "Stopping...");
		Running_ = false;

		GarbageCollector_.wakeUp();
		GarbageCollector_.join();

		if (WatcherId_ != 0) {
			KafkaManager()->UnregisterTopicWatcher(KafkaTopics::CNC_RES, WatcherId_);
			WatcherId_ = 0;
		}

		poco_information(Logger(), "Stopped...");
	}

	void AP_KAFKA_Server::OnKafkaMessage(const std::string &key, const std::string &payload) {
		if (!Running_) {
			return;
		}
		std::string forwardedPayload;
		try {
			Poco::JSON::Parser parser;
			Poco::JSON::Object::Ptr msg;
			msg = parser.parse(payload).extract<Poco::JSON::Object::Ptr>();
			if (msg->has("type")) {
				auto type = msg->get("type").toString();
				if (type == "infra_join") {
					return HandleInfraJoin(msg, key);
				}
				if (type == "infra_leave") {
					return HandleInfraLeave(msg, key);
				}
				poco_warning(Logger(), fmt::format("Message Type Invalid: {}",type));
				return;
			}

			return HandleDeviceMessage(msg, key, forwardedPayload);
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		}

	}

	void AP_KAFKA_Server::HandleInfraJoin(Poco::JSON::Object::Ptr msg, const std::string &key) {
		if (!msg->has("connect_message_payload")) {
			poco_warning(Logger(), "Infra_join missing 'connect'.");
			return;
		}
		auto ConnectPayload = msg->get("connect_message_payload").toString();
		auto IP = msg->has("infra_public_ip") ? msg->get("infra_public_ip").toString() : "";
		auto InfraSerial = msg->has("infra_group_infra") ? msg->get("infra_group_infra").toString() : "";
		if(ConnectPayload.empty() || IP.empty() || InfraSerial.empty())
		{
			poco_warning(Logger(), fmt::format("Infra_join empty field connectPayload: {} IP: {}, InfraSerial:{}", ConnectPayload,IP,InfraSerial));
			return;
		}
		Poco::JSON::Parser parser;
		auto connectParsed = parser.parse(ConnectPayload).extract<Poco::JSON::Object::Ptr>();
		if (!connectParsed || !connectParsed->isObject("params")) {
			poco_warning(Logger(), "Infra_join has invalid 'connect' payload.");
			return;
		}
		auto params = connectParsed->getObject("params");
		if (!params->has("serial")) {
			poco_warning(Logger(), "Infra_join connect payload missing params.serial.");
			return;
		}

		auto serial = Poco::trim(Poco::toLower(params->get("serial").toString())); 
		
		if (serial.empty()){
			poco_warning(Logger(), fmt::format("Infra_join serial empty for this IP: {}", IP));
			return;	
		}	
		
		if (!Utils::NormalizeMac(InfraSerial)) {
			poco_warning(Logger(), fmt::format("Infra_join Invalid infra_group_infra: {}", InfraSerial));
			return;
		}

		if(!Utils::ValidSerialNumber(serial)){
			poco_warning(Logger(), fmt::format("Infra_join Invalid serial: {}", serial));
			return;	
		}

		if (InfraSerial != serial) {
			poco_warning(Logger(), fmt::format("Infra_join serial mismatch: infra='{}' connect='{}'", InfraSerial, serial));
			return;
		}
		if (Connected(Utils::SerialNumberToInt(serial))) {
			poco_information(Logger(),
							 fmt::format("Infra_join: device already connected: {}", serial));
			return;
		}
		auto sessionId = ++session_id_;
		auto Session = std::make_shared<LockedDbSession>();
		auto NewConnection = std::make_shared<AP_KAFKA_Connection>( Logger(), Session, sessionId);
		AddConnection(NewConnection);
		NewConnection->Start();
		NewConnection->setEssentials(IP,InfraSerial);		
		NewConnection->ProcessIncomingPayload(ConnectPayload);
		poco_information(Logger(),
						 fmt::format("Infra_join: connected {} session={} key='{}'", serial, sessionId,
									 key));
	}

	void AP_KAFKA_Server::HandleInfraLeave(Poco::JSON::Object::Ptr msg, const std::string &key) {
		auto InfraSerial = msg->has("infra_group_infra") ? msg->get("infra_group_infra").toString() : "";
		if (InfraSerial.empty()){
			poco_warning(Logger(), fmt::format("Infra_leave serials empty"));
			return;
		}

		if (!Utils::NormalizeMac(InfraSerial)) {
			poco_warning(Logger(), fmt::format("Infra_join Invalid infra_group_infra: {}", InfraSerial));
			return;
		}

		if(!Utils::ValidSerialNumber(InfraSerial)){
			poco_warning(Logger(), fmt::format("Infra_join Invalid serial: {}", InfraSerial));
			return;	
		}

		auto serialInt = Utils::SerialNumberToInt(InfraSerial);
		if (!Connected(serialInt)) {
			poco_information(Logger(),
							 fmt::format("Infra_leave: Device Not Connected: {}", InfraSerial));
			return;
		}

		auto Conn = GetConnection(serialInt);
		if (!Conn) {
			poco_information(Logger(), fmt::format("Infra_leave:AP_Connection not found: {}", InfraSerial));
			return;
		}

		Conn->EndConnection();
		poco_information(Logger(), fmt::format("Infra_leave: disconnected {}", InfraSerial));
	}

	void AP_KAFKA_Server::HandleDeviceMessage(Poco::JSON::Object::Ptr msg, const std::string &key,
											 const std::string &rawPayload) {
		std::string serial;
		if (msg && msg->isObject("params")) {
			auto params = msg->getObject("params");
			if (params->has("serial")) {
			//	serial = StripSeparatorsToSerial(params->get("serial").toString());
			}
		}
		if (serial.empty()) {
			//serial = StripSeparatorsToSerial(key);
		}
		if (!Utils::ValidSerialNumber(serial)) {
			poco_warning(Logger(), fmt::format("Unroutable Kafka message key='{}'", key));
			return;
		}
		auto serialInt = Utils::SerialNumberToInt(serial);

		auto baseConn = GetConnection(serialInt);
		auto conn = std::dynamic_pointer_cast<AP_KAFKA_Connection>(baseConn);
		if (!conn) {
			poco_warning(Logger(), fmt::format("Kafka msg for non-connected device: {}", serial));
			return;
		}

		conn->ProcessIncomingPayload(rawPayload);
	}

	/*void AP_KAFKA_Server::run() {
		uint64_t last_log = Utils::Now(), last_zombie_run = 0, last_garbage_run = 0;

		Poco::Logger &LocalLogger =
			Poco::Logger::create("Kafka-Session-Janitor", Poco::Logger::root().getChannel(),
								 Poco::Logger::root().getLevel());

		while (Running_) {
			if (!Poco::Thread::trySleep(30000)) {
				break;
			}

			uint64_t total_connected_time = 0, now = Utils::Now();

			if (now - last_zombie_run > 60) {
				try {
					NumberOfConnectingDevices_ = 0;
					AverageDeviceConnectionTime_ = 0;

					int waits = 0;
					for (int hashIndex = 0; hashIndex < MACHash::HashMax(); hashIndex++) {
						last_zombie_run = now;
						waits = 0;
						while (true) {
							if (SerialNumbersMutex_[hashIndex].try_lock()) {
								waits = 0;
								auto hint = SerialNumbers_[hashIndex].begin();
								while (hint != end(SerialNumbers_[hashIndex])) {
									if (hint->second == nullptr) {
										hint = SerialNumbers_[hashIndex].erase(hint);
									} else {
										auto Device = hint->second;
										auto RightNow = Utils::Now();
										if (Device->Dead_) {
											AddCleanupSession(Device->State_.sessionId,
															  Device->SerialNumberInt_);
											++hint;
										} else if (RightNow > Device->LastContact_ &&
												   (RightNow - Device->LastContact_) >
													   SessionTimeOut_) {
											poco_information(
												LocalLogger,
												fmt::format(
													"{}: Session seems idle. Controller disconnecting device.",
													Device->SerialNumber_));
											AddCleanupSession(Device->State_.sessionId,
															  Device->SerialNumberInt_);
											++hint;
										} else {
											if (Device->State_.Connected) {
												total_connected_time +=
													(RightNow - Device->State_.started);
											}
											++hint;
										}
									}
								}
								SerialNumbersMutex_[hashIndex].unlock();
								break;
							} else if (waits < 5) {
								waits++;
								Poco::Thread::trySleep(10);
							} else {
								break;
							}
						}
					}

					LeftOverSessions_ = 0;
					for (int i = 0; i < SessionHash::HashMax(); i++) {
						waits = 0;
						while (true) {
							if (SessionMutex_[i].try_lock()) {
								waits = 0;
								auto hint = Sessions_[i].begin();
								auto RightNow = Utils::Now();
								while (hint != end(Sessions_[i])) {
									if (hint->second == nullptr) {
										hint = Sessions_[i].erase(hint);
									} else if (hint->second->Dead_) {
										AddCleanupSession(hint->second->State_.sessionId,
														  hint->second->SerialNumberInt_);
										++hint;
									} else if (RightNow > hint->second->LastContact_ &&
											   (RightNow - hint->second->LastContact_) >
												   SessionTimeOut_) {
										poco_information(
											LocalLogger,
											fmt::format(
												"{}: Session seems idle. Controller disconnecting device.",
												hint->second->SerialNumber_));
										AddCleanupSession(hint->second->State_.sessionId,
														  hint->second->SerialNumberInt_);
										++hint;
									} else {
										++LeftOverSessions_;
										++hint;
									}
								}
								SessionMutex_[i].unlock();
								break;
							} else if (waits < 5) {
								Poco::Thread::trySleep(10);
								waits++;
							} else {
								break;
							}
						}
					}

					AverageDeviceConnectionTime_ =
						NumberOfConnectedDevices_ > 0
							? total_connected_time / NumberOfConnectedDevices_
							: 0;
				} catch (const Poco::Exception &E) {
					poco_error(LocalLogger,
							   fmt::format("Poco::Exception: Garbage collecting failed: {}",
										   E.displayText()));
				} catch (const std::exception &E) {
					poco_error(LocalLogger,
							   fmt::format("std::exception: Garbage collecting failed: {}", E.what()));
				} catch (...) {
					poco_error(LocalLogger, "exception: Garbage collecting failed: unknown");
				}
			}

			if (NumberOfConnectedDevices_) {
				if (last_garbage_run > 0) {
					AverageDeviceConnectionTime_ += (now - last_garbage_run);
				}
			}

			try {
				if ((now - last_log) > 60) {
					last_log = now;
					poco_information(
						LocalLogger,
						fmt::format(
							"Active AP connections: {} Connecting: {} Average connection time: {} seconds. Left Over Sessions: {}",
							NumberOfConnectedDevices_, NumberOfConnectingDevices_,
							AverageDeviceConnectionTime_, LeftOverSessions_));
				}
				last_garbage_run = now;
			} catch (...) {
			}
		}
	}
	*/
} // namespace OpenWifi
