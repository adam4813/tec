#pragma once

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <thread>

#include <spdlog/spdlog.h>

#include "net-message.hpp"
#include "server-stats.hpp"

using asio::ip::tcp;

namespace tec {
namespace networking {
extern const std::string_view SERVER_PORT;
extern const std::string_view LOCAL_HOST;

const size_t PING_HISTORY_SIZE = 10;
const size_t DELAY_HISTORY_SIZE = 10;
typedef std::chrono::milliseconds::rep ping_time_t;
// std::chrono::milliseconds::rep is required to be signed and at least
// 45 bits, so it should be std::int64_t.
#define PRI_PING_TIME_T PRId64

// Used to connect to a server.
class ServerConnection {
public:
	// function to respond to incoming messages
	typedef std::function<void(MessageIn&)> messageHandlerFunc;

public:
	ServerConnection(ServerStats& s);

	bool Connect(std::string_view ip = LOCAL_HOST); // Connects to a server.

	void Disconnect(); // Closes the socket connection and stops the read and sync loops.

	void Stop(); // Stop all processing loops.

	void StartDispatch(); // Run the async dispatch loop.

	void StartSync(); // Run the sync loop.

	void SendChatMessage(std::string message); // Send a Message with type CHAT_MESSAGE.

	void Send(MessagePool::ptr_type msg);
	void Send(MessageOut& msg);
	void Send(MessageOut&& msg);

	// Gets the last received state ID.
	state_id_t GetLastRecvStateID() { return this->last_received_state_id; }

	// Get a list of recent pings.
	std::list<ping_time_t> GetRecentPings() {
		std::lock_guard<std::mutex> recent_ping_lock(recent_ping_mutex);
		return this->recent_pings;
	}

	// Returns the average ping.
	ping_time_t GetAveragePing() { return this->average_ping; }

	ping_time_t GetEstimatedDelay() {
		ping_time_t since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
										 std::chrono::high_resolution_clock::now() - recv_time)
										 .count();
		return (this->stats.estimated_server_time - this->stats.last_state_time) + since_recv;
	}
	// Get the client ID assigned by the server.
	eid GetClientID() { return this->client_id; }

	// Register a message handler for a given MessageType.
	void RegisterMessageHandler(MessageType type, messageHandlerFunc handler) {
		this->message_handlers[type].push_back(handler);
	}

	void RegisterConnectFunc(std::function<void()> func);

	size_t GetPartialMessageCount() const { return read_messages.size(); }

private:
	// These are used by the read loop:
	void read_header(); // Calls read_body after the header section is read.
	void read_body(); // Calls read_header after the whole body is read.
	// Async writes
	void do_write();

	void SyncHandler(Message::cptr_type message);
	void GameStateUpdateHandler(MessageIn& message);

	static std::shared_ptr<spdlog::logger> _log;

	// ASIO variables
	// we are only ever going to have one io_context
	static asio::io_context io_context;
	static asio::any_io_executor io_work;
	asio::ip::tcp::socket socket;

	// Async dispatch and sync loop variables
	MessagePool::ptr_type current_read_msg;
	std::map<uint32_t, std::unique_ptr<MessageIn>> read_messages;

	std::atomic<bool> run_dispatch;
	std::atomic<bool> run_sync;
	std::deque<MessagePool::ptr_type> write_msg_queue;
	static std::mutex write_msg_mutex;

	// Ping variables
	std::chrono::high_resolution_clock::time_point sync_start, recv_time;
	std::list<ping_time_t> recent_pings;
	static std::mutex recent_ping_mutex;
	ping_time_t average_ping{0};

	// Stats and Status
public:
	ServerStats& stats;

private:
	// Server-assigned client ID
	eid client_id{0};

	// State management variables
	state_id_t last_received_state_id{0};

	std::unordered_map<MessageType, std::list<messageHandlerFunc>> message_handlers;

	std::function<void()> onConnect = nullptr;
};
} // namespace networking
} // namespace tec
