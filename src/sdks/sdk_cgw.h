/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Helper for interacting with cgw-rest service.
//

#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>

#include "Poco/Dynamic/Var.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/Stringifier.h"
#include "Poco/Logger.h"
#include "Poco/Net/HTTPResponse.h"

#include "fmt/format.h"

#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "framework/utils.h"

namespace OpenWifi::SDK::CGW {

	inline std::string SerialToHyphenMac(const std::string &SerialNumber) {
		auto mac = Utils::SerialToMAC(SerialNumber);
		std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		std::replace(mac.begin(), mac.end(), ':', '-');
		return mac;
	}

	/**
	 * @brief Send a command to the CGW REST service and optionally return the device reply.
	 *
	 * Builds a `POST /api/v1/groups/{groupId}/infra/command` request, honours the provided timeout,
	 * and parses the response when the call is not marked as oneway.
	 *
	 * @param groupId Target CGW group identifier.
	 * @param serialNumber Device serial (converted to CGW MAC format for the REST body).
	 * @param method JSON-RPC method name to execute on the device.
	 * @param rpcPayload Prepared JSON-RPC payload to send through CGW.
	 * @param timeout Maximum time to wait for the REST request and optional response payload.
	 * @param oneway When true, do not expect a synchronous payload back from CGW.
	 * @param responseOut Populated with the parsed device response when `oneway == false`.
	 * @param logger Logger used for informational and warning messages.
	 * @return true if the REST request succeeded and (for non-oneway) a payload was extracted.
	 */
	inline bool PostInfraCommand(
		const std::string &groupId, const std::string &serialNumber, const std::string &method,
		const Poco::JSON::Object::Ptr &rpcPayload, std::chrono::milliseconds timeout, bool oneway,
		Poco::JSON::Object::Ptr &responseOut, Poco::Logger &logger) {

		responseOut = nullptr;

		if (timeout.count() <= 0)
			timeout = std::chrono::milliseconds(30000);

		int timeoutSeconds = std::max<int>(
			1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));

		Poco::JSON::Object body;
		body.set("mac_addr", SerialToHyphenMac(serialNumber));
		body.set("method", method);
		body.set("timeout", timeoutSeconds);
		body.set("params", rpcPayload);

		auto endpointPath = fmt::format("/api/v1/groups/{}/infra/command", groupId);
		std::ostringstream serializedBodyStream;
		Poco::JSON::Stringifier::stringify(body, serializedBodyStream);
		poco_information(logger,
				fmt::format("CGW REST POST {} payload={}", endpointPath, serializedBodyStream.str()));

		Poco::JSON::Object::Ptr restResponse;
		OpenAPIRequestPost request(
			uSERVICE_CGWREST, endpointPath, {}, body,
			static_cast<uint64_t>(timeout.count()),
			fmt::format("CGW command {} {}", serialNumber, method));

		auto status = request.Do(restResponse);
		poco_information(logger,
				fmt::format("CGW REST POST {} status={}", endpointPath,
						static_cast<int>(status)));
		if (status < Poco::Net::HTTPResponse::HTTP_OK ||
			status >= Poco::Net::HTTPResponse::HTTP_MULTIPLE_CHOICES) {
			poco_warning(logger, fmt::format(
				"REST command {} for {} failed. HTTP status {}", method, serialNumber,
				static_cast<int>(status)));
			return false;
		}

		if (oneway)
			return true;

		if (restResponse.isNull()) {
			poco_warning(logger, fmt::format(
				"REST command {} for {} returned empty payload.", method, serialNumber));
			return false;
		}

		Poco::Dynamic::Var payloadVar;
		if (restResponse->has("received_payload")) {
			payloadVar = restResponse->get("received_payload");
		}
		else {
			poco_warning(logger, fmt::format(
				"REST command {} for {} missing received_payload field.", method, serialNumber));
			return false;
		}

		try {
			if (payloadVar.type() == typeid(Poco::JSON::Object::Ptr)) {
				responseOut = payloadVar.extract<Poco::JSON::Object::Ptr>();
			} else {
				auto payloadString = payloadVar.convert<std::string>();
				Poco::JSON::Parser parser;
				Poco::Dynamic::Var innerVar = parser.parse(payloadString);
				responseOut = innerVar.extract<Poco::JSON::Object::Ptr>();
			}
		} catch (const Poco::Exception &E) {
			poco_warning(logger, fmt::format(
				"REST command {} for {} returned unparsable payload: {}", method, serialNumber,
				E.displayText()));
			return false;
		} catch (const std::exception &E) {
			poco_warning(logger, fmt::format(
				"REST command {} for {} returned unparsable payload: {}", method, serialNumber,
				E.what()));
			return false;
		}

		return !responseOut.isNull();
	}

} // namespace OpenWifi::SDK::CGW
