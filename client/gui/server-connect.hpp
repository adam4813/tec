#pragma once

#include <sstream>

#include <imgui.h>

#include "abs_window.hpp"
#include "server-connection.hpp"

namespace tec {
using networking::ServerConnection;
class IMGUISystem;

class ServerConnectWindow : public AbstractWindow {
public:
	ServerConnectWindow(ServerConnection& server_connection);

	void Update(double) override;

	void Draw(IMGUISystem*) override;

private:
	ServerConnection& server_connection;
};

class PingTimesWindow : public AbstractWindow {
public:
	PingTimesWindow(ServerConnection& server_connection);

	void Update(double) override;

	void Draw(IMGUISystem*) override;

private:
	ServerConnection& server_connection;
};
} // namespace tec
