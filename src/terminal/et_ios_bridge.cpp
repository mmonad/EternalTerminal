//
// et_ios_bridge.cpp
// C bridge for calling ET client from iOS (Swift via bridging header).
// Modeled after TerminalClient::run() but uses pipe file descriptors
// instead of a Console object, matching the mosh_main() pattern.
//

#include "et_ios_bridge.h"

#include "ClientConnection.hpp"
#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "TcpSocketHandler.hpp"

using namespace et;

// Maximum read buffer size (matches TerminalClient)
#define ET_BUF_SIZE (16 * 1024)

extern "C" int et_client_main(FILE *f_in, FILE *f_out, struct winsize *ws,
                               const char *host, int port, const char *id,
                               const char *passkey, int keepalive_secs) {
  // Validate parameters
  if (!f_in || !f_out || !ws || !host || !id || !passkey) {
    return 1;
  }
  if (keepalive_secs < 1) {
    keepalive_secs = 5;
  }

  // Prevent easylogging++ from calling abort() on FATAL log messages.
  // ET's reconnect thread (ClientConnection::pollReconnect) can trigger
  // FATAL logs when the server-side daemon is gone, which would crash
  // the entire iOS process.
  el::Loggers::addFlag(el::LoggingFlag::DisableApplicationAbortOnFatalLog);

  try {
    // Create socket handler and endpoint
    auto socketHandler = std::make_shared<TcpSocketHandler>();
    SocketEndpoint endpoint;
    endpoint.set_name(std::string(host));
    endpoint.set_port(port);

    // Create client connection
    auto connection = std::make_shared<ClientConnection>(
        socketHandler, endpoint, std::string(id), std::string(passkey));

    // Connect with retry (up to 3 attempts, matching TerminalClient)
    int connectFailCount = 0;
    while (true) {
      try {
        bool fail = true;
        if (connection->connect()) {
          // Send initial payload (no jumphost, no tunnels)
          InitialPayload payload;
          payload.set_jumphost(false);
          connection->writePacket(
              Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));

          // Wait for initial response
          fd_set rfd;
          timeval tv;
          for (int a = 0; a < 3; a++) {
            FD_ZERO(&rfd);
            int clientFd = connection->getSocketFd();
            if (clientFd < 0) {
              std::this_thread::sleep_for(std::chrono::seconds(1));
              continue;
            }
            FD_SET(clientFd, &rfd);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            select(clientFd + 1, &rfd, NULL, NULL, &tv);
            if (FD_ISSET(clientFd, &rfd)) {
              Packet initialResponsePacket;
              if (connection->readPacket(&initialResponsePacket)) {
                if (initialResponsePacket.getHeader() !=
                    EtPacketType::INITIAL_RESPONSE) {
                  return 1;
                }
                auto initialResponse = stringToProto<InitialResponse>(
                    initialResponsePacket.getPayload());
                if (initialResponse.has_error()) {
                  return 1;
                }
                fail = false;
                break;
              }
            }
          }
        }
        if (fail) {
          connectFailCount++;
          if (connectFailCount >= 3) {
            return 1;
          }
          continue;
        }
      } catch (...) {
        return 1;
      }
      break;
    }

    // Get file descriptors for select()
    int inFd = fileno(f_in);
    int outFd = fileno(f_out);

    // Track terminal size for change detection
    TerminalInfo lastTerminalInfo;

    time_t keepaliveTime = time(NULL) + keepalive_secs;
    bool waitingOnKeepalive = false;

    char buf[ET_BUF_SIZE];

    // Main loop (modeled after TerminalClient::run())
    while (!connection->isShuttingDown()) {
      fd_set rfd;
      timeval tv;

      FD_ZERO(&rfd);
      int maxfd = inFd;
      FD_SET(inFd, &rfd);

      int clientFd = connection->getSocketFd();
      if (clientFd > 0) {
        FD_SET(clientFd, &rfd);
        if (clientFd > maxfd) maxfd = clientFd;
      }

      tv.tv_sec = 0;
      tv.tv_usec = 10000;  // 10ms timeout
      select(maxfd + 1, &rfd, NULL, NULL, &tv);

      try {
        // Check for user input from pipe
        if (FD_ISSET(inFd, &rfd)) {
          int rc = ::read(inFd, buf, ET_BUF_SIZE);
          if (rc <= 0) {
            // Pipe closed or error — exit
            break;
          }
          std::string s(buf, rc);
          TerminalBuffer tb;
          tb.set_buffer(s);
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          keepaliveTime = time(NULL) + keepalive_secs;
        }

        // Check for data from server
        if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
          while (connection->hasData()) {
            Packet packet;
            if (!connection->read(&packet)) {
              break;
            }
            uint8_t packetType = packet.getHeader();
            switch (packetType) {
              case TerminalPacketType::TERMINAL_BUFFER: {
                TerminalBuffer tb =
                    stringToProto<TerminalBuffer>(packet.getPayload());
                const std::string &s = tb.buffer();
                // Write to output pipe
                size_t written = 0;
                while (written < s.length()) {
                  ssize_t rc =
                      ::write(outFd, s.c_str() + written, s.length() - written);
                  if (rc <= 0) break;
                  written += rc;
                }
                keepaliveTime = time(NULL) + keepalive_secs;
                break;
              }
              case TerminalPacketType::KEEP_ALIVE:
                waitingOnKeepalive = false;
                break;
              default:
                // Ignore port forwarding and other packet types
                break;
            }
          }
        }

        // Keepalive
        if (clientFd > 0 && keepaliveTime < time(NULL)) {
          keepaliveTime = time(NULL) + keepalive_secs;
          if (waitingOnKeepalive) {
            connection->closeSocketAndMaybeReconnect();
            waitingOnKeepalive = false;
          } else {
            connection->writePacket(
                Packet(TerminalPacketType::KEEP_ALIVE, ""));
            waitingOnKeepalive = true;
          }
        }
        if (clientFd < 0) {
          waitingOnKeepalive = false;
        }

        // Check for terminal size changes
        if (ws) {
          TerminalInfo ti;
          ti.set_row(ws->ws_row);
          ti.set_column(ws->ws_col);
          ti.set_width(ws->ws_xpixel);
          ti.set_height(ws->ws_ypixel);

          if (ti.row() != lastTerminalInfo.row() ||
              ti.column() != lastTerminalInfo.column() ||
              ti.width() != lastTerminalInfo.width() ||
              ti.height() != lastTerminalInfo.height()) {
            lastTerminalInfo = ti;
            connection->writePacket(
                Packet(TerminalPacketType::TERMINAL_INFO, protoToString(ti)));
          }
        }
      } catch (const std::runtime_error &re) {
        // Connection error — exit
        break;
      }
    }

    connection->shutdown();
  } catch (...) {
    return 1;
  }

  return 0;
}
