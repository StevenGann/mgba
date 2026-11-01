/* Copyright (c) 2026 Steven Gann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SERVAL_EMU_LINK_H
#define SERVAL_EMU_LINK_H

// The endpoint of Serval Engine's debug link, protocol 1.0 and mailbox v1
// (Serval Engine's docs/debug-link.md): a TCP connection the endpoint dials
// to the debugger (Studio Advance), the agent's mailbox in EWRAM reached
// through the core's memory, and the doorbell (the register hook in
// serval-link.h). It answers the endpoint's own messages (READ_MEM,
// WRITE_MEM, MACHINE_PAUSE, MACHINE_RESUME, LOG) and carries the agent's
// both ways without reading them.
//
// Single-threaded: the frontend calls ServalLinkService() between the core's
// run loops, and the core calls the ring during a doorbell write.

#include <mgba-util/common.h>
#include <mgba-util/socket.h>

#include <mgba/internal/gba/serval-link.h>

struct mCore;
struct GBA;

enum {
	SERVAL_LINK_LOG_INFO = 0,
	SERVAL_LINK_LOG_WARNING = 1,
	SERVAL_LINK_LOG_ERROR = 2,

	SERVAL_LINK_SOURCE_GAME = 0,   // the game's debug_log()
	SERVAL_LINK_SOURCE_ENGINE = 1, // an engine warning ("serval:")
	SERVAL_LINK_SOURCE_EMULATOR = 2,
};

#define SERVAL_LINK_LOG_BACKLOG 256

struct ServalLinkLogLine {
	uint8_t level;
	uint8_t source;
	uint32_t frame;
	char text[256];
};

struct ServalLink {
	struct GBAServalLink d; // the core's hook; first, so the ring finds us
	struct mCore* core;
	struct GBA* gba;
	const char* version; // ours, for HELLO

	Socket socket;
	bool connected;  // a TCP connection is open
	bool handshaken; // the debugger answered our HELLO
	char token[256];

	uint8_t rx[2048]; // bytes received, not yet a whole frame
	size_t rxLength;

	uint8_t* pending; // whole frames for the agent, waiting for its inbox
	size_t pendingLength;
	size_t pendingCapacity;

	uint32_t mailbox;    // the agent's mailbox address (0: none yet)
	bool agentFoundSent; // AGENT_FOUND sent for this agent
	bool agentParked;    // the agent rang saying parked: the machine waits
	bool machinePaused;  // MACHINE_PAUSE

	struct ServalLinkLogLine backlog[SERVAL_LINK_LOG_BACKLOG]; // LOG before the handshake
	size_t backlogStart;
	size_t backlogCount;
};

// Installs the register hook on the core (a GBA core). Without a connection,
// the endpoint still answers the presence register, so the agent starts;
// nothing parks without a debugger.
void ServalLinkInit(struct ServalLink*, struct mCore*, const char* version);
// Dials host:port (127.0.0.1:PORT), sends HELLO with the token. False, with
// a message on stderr, if it can't connect.
bool ServalLinkConnect(struct ServalLink*, const char* hostPort, const char* token);
void ServalLinkDeinit(struct ServalLink*);

// Reads the socket (waiting up to timeoutMs for something to arrive),
// answers the endpoint's messages, and hands the agent its frames when its
// inbox is free.
void ServalLinkService(struct ServalLink*, int timeoutMs);
// Whether the machine may run: not paused by the debugger, not parked by
// the agent.
bool ServalLinkMachineRuns(const struct ServalLink*);
// A LOG event: sent now, or kept (the last 256) until the handshake.
void ServalLinkLog(struct ServalLink*, int level, int source, const char* text);

#endif
