/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Transport layer for AP WebSocket connections
// Extracted to separate WebSocket-only I/O handler.

#pragma once

#include <memory>
#include <string>

#include "Poco/Buffer.h"
#include "Poco/Logger.h"
#include "Poco/Net/SocketNotification.h"
#include "Poco/Net/SocketReactor.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/WebSocket.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"

namespace OpenWifi {

    // Listener interface implemented by the processing layer (AP_WS_Connection)
    class AP_WS_TransportListener {
      public:
        virtual ~AP_WS_TransportListener() = default;

        virtual bool IsServerRunning() const = 0;
        virtual bool IsDeviceValidated() const = 0;
        virtual bool TryValidateDevice(Poco::Net::WebSocket &ws) = 0;

        // Record transport metrics on received/sent bytes and message counts
        virtual void RecordFrameReceived(std::size_t bytes) = 0;
        virtual void RecordFrameSent(std::size_t bytes) = 0;
        virtual void RecordMessageCount() = 0;
        virtual void UpdateLastContact() = 0;

        // Notification hooks
        virtual void OnPing() = 0;             // transport answered with PONG
        virtual void OnTextFrame(const std::string &payload) = 0; // application data
        virtual void OnTransportClosed() = 0;   // socket closed/disconnected
        virtual Poco::Logger &Logger() = 0;     // for logging
    };

    // WebSocket-only transport: manages lifecycle and I/O, delegates processing to listener
    class AP_WS_Transport {
      
      public:
      /**
       * @brief Construct the transport by performing the HTTP upgrade and configuring the socket.
       *
       * Takes ownership of the WebSocket, sets non-blocking mode/timeouts, and keeps references
       * to the reactor and listener for subsequent event handling.
       */     
        AP_WS_Transport(Poco::Net::HTTPServerRequest &request,
                        Poco::Net::HTTPServerResponse &response,
                        std::shared_ptr<Poco::Net::SocketReactor> reactor,
                        AP_WS_TransportListener &listener,
                        Poco::Logger &logger);

        ~AP_WS_Transport();

        void Start();
        void Shutdown();

        // Send text payload over websocket
        bool Send(const std::string &payload);

        inline Poco::Net::WebSocket &Socket() { return *ws_; }

        // Reactor handlers (registered internally)
        void OnSocketReadable(const Poco::AutoPtr<Poco::Net::ReadableNotification> &pNf);
        void OnSocketShutdown(const Poco::AutoPtr<Poco::Net::ShutdownNotification> &pNf);
        void OnSocketError(const Poco::AutoPtr<Poco::Net::ErrorNotification> &pNf);

      private:
      /**
       * @brief Internal helper that reads and interprets a single WebSocket frame.
       *
       * Updates accounting, handles control frames (PING/PONG/CLOSE), and forwards text frames
       * to the listener.
       */
        void ProcessIncomingFrame();

        std::shared_ptr<Poco::Net::SocketReactor> reactor_;
        std::unique_ptr<Poco::Net::WebSocket> ws_;
        AP_WS_TransportListener &listener_;
        Poco::Logger &logger_;
        bool registered_{false};

        static constexpr int BufSize = 256000;
    };

} // namespace OpenWifi
