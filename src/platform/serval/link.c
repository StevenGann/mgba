/* Copyright (c) 2026 Steven Gann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "link.h"

#include <mgba/core/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- The protocol (Serval Engine's docs/debug-link.md) ---------------------

#define PROTO_MAJOR 1
#define PROTO_MINOR 0
#define KIND_MGBA_FORK 1

#define FRAME_MAX 1024 // a whole frame, its length field included
#define HEADER 6       // length u16, type u8, flags u8, seq u16
#define FLAG_REPLY 1
#define FLAG_ERROR 2

enum {
	MSG_HELLO = 0x01,
	MSG_READ_MEM = 0x02,
	MSG_WRITE_MEM = 0x03,
	MSG_MACHINE_PAUSE = 0x04,
	MSG_MACHINE_RESUME = 0x05,
	MSG_LOG = 0x20,
	MSG_GAME_RESET = 0x21,
	MSG_AGENT_FOUND = 0x22,
	MSG_FIRST_AGENT = 0x40,
};

enum {
	ERR_UNSUPPORTED = 1,
	ERR_BAD_REQUEST = 2,
	ERR_NO_AGENT = 3,
};

// The endpoint's capabilities: raw memory, machine pause, logs.
#define CAPABILITIES 0x7u

// --- Mailbox v1 -------------------------------------------------------------

#define MB_MAGIC 0
#define MB_VERSION 4
#define MB_SIZE 6
#define MB_ENDPOINT 8
#define MB_IN_LEN 12
#define MB_OUT_LEN 14
#define MB_STATE 16
#define MB_IN 20
#define MB_IN_SIZE 1024
#define MB_OUT 1044
#define MB_OUT_SIZE 2048
#define MB_V1_SIZE 3092

#define MAGIC_SVLK 0x4B4C5653u // the bytes "SVLK"
#define MAGIC_SVLE 0x454C5653u // the bytes "SVLE"

#define PENDING_MAX (256 * 1024)

static uint16_t _get16(const uint8_t* p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t _get32(const uint8_t* p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void _put16(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void _put32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

// The mailbox in EWRAM, or NULL if address can't hold one.
static uint8_t* _mailboxAt(struct ServalLink* link, uint32_t address) {
	if ((address >> 24) != 0x02 || (address & 3)) {
		return NULL;
	}
	uint32_t offset = address & (GBA_SIZE_EWRAM - 1);
	if (offset + MB_V1_SIZE > GBA_SIZE_EWRAM) {
		return NULL;
	}
	return (uint8_t*) link->gba->memory.wram + offset;
}

static uint8_t* _mailbox(struct ServalLink* link) {
	return link->mailbox ? _mailboxAt(link, link->mailbox) : NULL;
}

// --- Sending ------------------------------------------------------------------

static void _disconnect(struct ServalLink* link, const char* why);

static bool _sendBytes(struct ServalLink* link, const uint8_t* bytes, size_t length) {
	if (!link->connected) {
		return false;
	}
	while (length) {
		ssize_t sent = SocketSend(link->socket, bytes, length);
		if (sent > 0) {
			bytes += sent;
			length -= (size_t) sent;
			continue;
		}
		if (sent < 0 && SocketWouldBlock()) {
			Socket writes[1] = { link->socket };
			if (SocketPoll(1, NULL, writes, NULL, 5000) <= 0) {
				_disconnect(link, "the debugger stopped reading");
				return false;
			}
			continue;
		}
		_disconnect(link, "the connection broke while sending");
		return false;
	}
	return true;
}

static void _sendFrame(struct ServalLink* link, int type, int flags, unsigned seq, const uint8_t* payload, size_t length) {
	uint8_t frame[FRAME_MAX];
	if (length > FRAME_MAX - HEADER) {
		length = FRAME_MAX - HEADER;
	}
	_put16(frame, (uint32_t) (4 + length));
	frame[2] = (uint8_t) type;
	frame[3] = (uint8_t) flags;
	_put16(frame + 4, seq);
	if (length) {
		memcpy(frame + HEADER, payload, length);
	}
	_sendBytes(link, frame, HEADER + length);
}

static void _sendError(struct ServalLink* link, int type, unsigned seq, int code, const char* text) {
	uint8_t payload[2 + 255];
	size_t n = strlen(text);
	if (n > 255) {
		n = 255;
	}
	payload[0] = (uint8_t) code;
	payload[1] = (uint8_t) n;
	memcpy(payload + 2, text, n);
	_sendFrame(link, type, FLAG_REPLY | FLAG_ERROR, seq, payload, 2 + n);
}

static size_t _putStr(uint8_t* p, const char* s) {
	size_t n = strlen(s);
	if (n > 255) {
		n = 255;
	}
	p[0] = (uint8_t) n;
	memcpy(p + 1, s, n);
	return 1 + n;
}

static void _sendHello(struct ServalLink* link) {
	uint8_t payload[3 + 256 + 256 + 4];
	size_t n = 0;
	payload[n++] = PROTO_MAJOR;
	payload[n++] = PROTO_MINOR;
	payload[n++] = KIND_MGBA_FORK;
	n += _putStr(payload + n, link->version);
	n += _putStr(payload + n, link->token);
	_put32(payload + n, CAPABILITIES);
	n += 4;
	_sendFrame(link, MSG_HELLO, 0, 0, payload, n);
}

static void _sendLog(struct ServalLink* link, const struct ServalLinkLogLine* line) {
	uint8_t payload[2 + 4 + 256];
	size_t n = 0;
	payload[n++] = line->level;
	payload[n++] = line->source;
	_put32(payload + n, line->frame);
	n += 4;
	n += _putStr(payload + n, line->text);
	_sendFrame(link, MSG_LOG, 0, 0, payload, n);
}

// --- The agent ------------------------------------------------------------------

// Tells the debugger the agent is there, and marks the mailbox as connected.
static void _announceAgent(struct ServalLink* link, uint8_t* m) {
	_sendFrame(link, MSG_AGENT_FOUND, 0, 0, NULL, 0);
	_put32(m + MB_ENDPOINT, MAGIC_SVLE);
	link->agentFoundSent = true;
}

// Moves whole frames from the pending queue into the agent's free inbox.
static void _deliver(struct ServalLink* link, uint8_t* m) {
	size_t n = 0;
	while (n + 2 <= link->pendingLength) {
		size_t frame = 2 + _get16(link->pending + n);
		if (n + frame > MB_IN_SIZE) {
			break;
		}
		n += frame;
	}
	if (!n) {
		return;
	}
	memcpy(m + MB_IN, link->pending, n);
	_put16(m + MB_IN_LEN, (uint32_t) n);
	memmove(link->pending, link->pending + n, link->pendingLength - n);
	link->pendingLength -= n;
}

static bool _canDeliver(struct ServalLink* link, uint8_t* m) {
	return m && link->pendingLength && !link->machinePaused && _get16(m + MB_IN_LEN) == 0;
}

// The doorbell: during the agent's write of its mailbox's address.
static void _ring(struct GBAServalLink* hook, struct GBA* gba, uint32_t address) {
	struct ServalLink* link = (struct ServalLink*) hook;
	uint8_t* m = _mailboxAt(link, address);
	if (!m || _get32(m + MB_MAGIC) != MAGIC_SVLK || _get16(m + MB_VERSION) < 1 || _get16(m + MB_SIZE) < MB_V1_SIZE) {
		static bool warned = false;
		if (!warned) {
			fprintf(stderr, "serval-emu: a ring with no valid mailbox at 0x%08X: ignored\n", address);
			warned = true;
		}
		return;
	}
	bool fresh = link->mailbox != address;
	link->mailbox = address;

	// The agent zeroes its mailbox when it starts: SVLE gone means it started again.
	if (link->handshaken && (fresh || _get32(m + MB_ENDPOINT) != MAGIC_SVLE)) {
		if (link->agentFoundSent) {
			_sendFrame(link, MSG_GAME_RESET, 0, 0, NULL, 0);
			link->pendingLength = 0; // requests for the agent that went
		}
		_announceAgent(link, m);
	}

	uint16_t outLength = _get16(m + MB_OUT_LEN);
	if (outLength) {
		if (outLength <= MB_OUT_SIZE && link->handshaken) {
			_sendBytes(link, m + MB_OUT, outLength);
		}
		_put16(m + MB_OUT_LEN, 0);
	}

	if (m[MB_STATE] == 0) {
		link->agentParked = false;
		return;
	}
	if (_canDeliver(link, m)) {
		_deliver(link, m); // a request waits already: the agent goes on to read it
		link->agentParked = false;
		return;
	}
	// Parked: stop the machine right after this store, until a frame comes.
	link->agentParked = true;
	gba->cpu->nextEvent = gba->cpu->cycles;
	GBAInterrupt(gba);
}

// --- Receiving ------------------------------------------------------------------

static void _flushBacklog(struct ServalLink* link) {
	for (size_t i = 0; i < link->backlogCount && link->connected; ++i) {
		_sendLog(link, &link->backlog[(link->backlogStart + i) % SERVAL_LINK_LOG_BACKLOG]);
	}
	link->backlogStart = 0;
	link->backlogCount = 0;
}

static void _queue(struct ServalLink* link, const uint8_t* frame, size_t length) {
	if (link->pendingLength + length > PENDING_MAX) {
		_sendError(link, frame[2], _get16(frame + 4), ERR_BAD_REQUEST, "the endpoint's queue for the agent is full");
		return;
	}
	if (link->pendingLength + length > link->pendingCapacity) {
		size_t capacity = link->pendingCapacity ? link->pendingCapacity * 2 : 4096;
		while (capacity < link->pendingLength + length) {
			capacity *= 2;
		}
		link->pending = realloc(link->pending, capacity);
		link->pendingCapacity = capacity;
	}
	memcpy(link->pending + link->pendingLength, frame, length);
	link->pendingLength += length;
}

static void _handleEndpointRequest(struct ServalLink* link, int type, unsigned seq, const uint8_t* p, size_t n) {
	struct mCore* core = link->core;
	switch (type) {
	case MSG_READ_MEM: {
		if (n < 6) {
			_sendError(link, type, seq, ERR_BAD_REQUEST, "READ_MEM: address u32, length u16");
			return;
		}
		uint32_t address = _get32(p);
		unsigned length = _get16(p + 4);
		if (length < 1 || length > FRAME_MAX - HEADER - 2) {
			_sendError(link, type, seq, ERR_BAD_REQUEST, "READ_MEM: a length of 1 to 1016 bytes");
			return;
		}
		uint8_t bytes[FRAME_MAX];
		for (unsigned i = 0; i < length; ++i) {
			bytes[i] = (uint8_t) core->rawRead8(core, address + i, -1);
		}
		_sendFrame(link, type, FLAG_REPLY, seq, bytes, length);
		return;
	}
	case MSG_WRITE_MEM: {
		if (n < 4) {
			_sendError(link, type, seq, ERR_BAD_REQUEST, "WRITE_MEM: address u32, then the bytes");
			return;
		}
		uint32_t address = _get32(p);
		for (size_t i = 4; i < n; ++i) {
			core->rawWrite8(core, address + (uint32_t) (i - 4), -1, p[i]);
		}
		_sendFrame(link, type, FLAG_REPLY, seq, NULL, 0);
		return;
	}
	case MSG_MACHINE_PAUSE:
		link->machinePaused = true;
		_sendFrame(link, type, FLAG_REPLY, seq, NULL, 0);
		return;
	case MSG_MACHINE_RESUME:
		link->machinePaused = false;
		_sendFrame(link, type, FLAG_REPLY, seq, NULL, 0);
		return;
	default:
		_sendError(link, type, seq, ERR_UNSUPPORTED, "the endpoint doesn't know this request");
		return;
	}
}

static void _handleFrame(struct ServalLink* link, const uint8_t* frame, size_t length) {
	int type = frame[2];
	int flags = frame[3];
	unsigned seq = _get16(frame + 4);
	const uint8_t* payload = frame + HEADER;
	size_t n = length - HEADER;

	if (!link->handshaken) {
		if (type != MSG_HELLO || n < 3) {
			_disconnect(link, "the debugger's first frame wasn't HELLO");
			return;
		}
		if (payload[0] != PROTO_MAJOR) {
			fprintf(stderr, "serval-emu: the debugger speaks protocol %u.%u; this endpoint speaks %u.%u\n", payload[0], payload[1], PROTO_MAJOR, PROTO_MINOR);
			_disconnect(link, "protocol major mismatch");
			return;
		}
		link->handshaken = true;
		_flushBacklog(link);
		uint8_t* m = _mailbox(link);
		if (m) {
			_announceAgent(link, m);
		}
		return;
	}
	if ((flags & FLAG_REPLY) || seq == 0) {
		return; // a reply or an event from the debugger: no reply (protocol 1.0 has none)
	}
	if (type < MSG_FIRST_AGENT) {
		_handleEndpointRequest(link, type, seq, payload, n);
		return;
	}
	if (!_mailbox(link)) {
		_sendError(link, type, seq, ERR_NO_AGENT, "no agent has rung: a release build, an engine without the agent, or a game that hasn't started");
		return;
	}
	_queue(link, frame, length);
}

static void _receive(struct ServalLink* link, int timeoutMs) {
	while (link->connected) {
		Socket reads[1] = { link->socket };
		if (SocketPoll(1, reads, NULL, NULL, timeoutMs) <= 0) {
			return;
		}
		timeoutMs = 0;
		if (link->rxLength >= sizeof(link->rx)) { // can't happen: whole frames are consumed below
			_disconnect(link, "the receive buffer overflowed");
			return;
		}
		ssize_t got = SocketRecv(link->socket, link->rx + link->rxLength, sizeof(link->rx) - link->rxLength);
		if (got == 0 || (got < 0 && !SocketWouldBlock())) {
			_disconnect(link, "the debugger closed the link");
			return;
		}
		if (got < 0) {
			return;
		}
		link->rxLength += (size_t) got;
		size_t at = 0;
		while (link->connected && link->rxLength - at >= 2) {
			size_t frameLength = _get16(link->rx + at);
			if (frameLength < 4 || frameLength > FRAME_MAX - 2) {
				_disconnect(link, "a broken frame from the debugger");
				return;
			}
			if (link->rxLength - at < 2 + frameLength) {
				break;
			}
			_handleFrame(link, link->rx + at, 2 + frameLength);
			at += 2 + frameLength;
		}
		if (!link->connected) {
			return;
		}
		memmove(link->rx, link->rx + at, link->rxLength - at);
		link->rxLength -= at;
	}
}

static void _disconnect(struct ServalLink* link, const char* why) {
	if (!link->connected) {
		return;
	}
	fprintf(stderr, "serval-emu: %s; the game runs on without a debugger\n", why);
	SocketClose(link->socket);
	link->socket = INVALID_SOCKET;
	link->connected = false;
	link->handshaken = false;
	link->rxLength = 0;
	link->pendingLength = 0;
	link->agentFoundSent = false;
	link->machinePaused = false;
	// The agent detaches when the endpoint field goes, resuming a parked game.
	uint8_t* m = _mailbox(link);
	if (m) {
		_put32(m + MB_ENDPOINT, 0);
	}
	link->agentParked = false;
}

// --- The frontend's side --------------------------------------------------------

void ServalLinkInit(struct ServalLink* link, struct mCore* core, const char* version) {
	memset(link, 0, sizeof(*link));
	link->d.ring = _ring;
	link->core = core;
	link->gba = core->board;
	link->version = version;
	link->socket = INVALID_SOCKET;
	link->gba->servalLink = &link->d;
}

bool ServalLinkConnect(struct ServalLink* link, const char* hostPort, const char* token) {
	const char* colon = strrchr(hostPort, ':');
	if (!colon) {
		fprintf(stderr, "serval-emu: --link wants HOST:PORT, not %s\n", hostPort);
		return false;
	}
	char host[64];
	size_t hostLength = (size_t) (colon - hostPort);
	if (hostLength >= sizeof(host)) {
		fprintf(stderr, "serval-emu: --link: host too long\n");
		return false;
	}
	memcpy(host, hostPort, hostLength);
	host[hostLength] = '\0';
	unsigned a, b, c, d;
	struct Address address = { .version = IPV4 };
	if (!strcmp(host, "localhost")) {
		address.ipv4 = 0x7F000001;
	} else if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
		address.ipv4 = (a << 24) | (b << 16) | (c << 8) | d;
	} else {
		fprintf(stderr, "serval-emu: --link: an IPv4 address, such as 127.0.0.1, not %s\n", host);
		return false;
	}
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535) {
		fprintf(stderr, "serval-emu: --link: no port in %s\n", hostPort);
		return false;
	}
	snprintf(link->token, sizeof(link->token), "%s", token ? token : "");

	SocketSubsystemInit();
	Socket socket = SocketConnectTCP(port, &address);
	if (SOCKET_FAILED(socket)) {
		fprintf(stderr, "serval-emu: can't reach the debugger at %s\n", hostPort);
		return false;
	}
	SocketSetTCPPush(socket, 1); // TCP_NODELAY: small requests and replies
	SocketSetBlocking(socket, false);
	link->socket = socket;
	link->connected = true;
	_sendHello(link);
	return link->connected;
}

void ServalLinkDeinit(struct ServalLink* link) {
	if (link->connected) {
		SocketClose(link->socket);
		link->connected = false;
	}
	if (link->gba && link->gba->servalLink == &link->d) {
		link->gba->servalLink = NULL;
	}
	free(link->pending);
	link->pending = NULL;
}

void ServalLinkService(struct ServalLink* link, int timeoutMs) {
	_receive(link, timeoutMs);
	uint8_t* m = _mailbox(link);
	if (_canDeliver(link, m)) {
		_deliver(link, m);
		link->agentParked = false; // the machine runs until the agent's next ring
	}
}

bool ServalLinkMachineRuns(const struct ServalLink* link) {
	return !link->machinePaused && !link->agentParked;
}

void ServalLinkLog(struct ServalLink* link, int level, int source, const char* text) {
	struct ServalLinkLogLine line;
	line.level = (uint8_t) level;
	line.source = (uint8_t) source;
	line.frame = link->core->frameCounter(link->core);
	snprintf(line.text, sizeof(line.text), "%s", text);
	if (link->handshaken) {
		_sendLog(link, &line);
		return;
	}
	size_t slot;
	if (link->backlogCount < SERVAL_LINK_LOG_BACKLOG) {
		slot = (link->backlogStart + link->backlogCount++) % SERVAL_LINK_LOG_BACKLOG;
	} else {
		slot = link->backlogStart;
		link->backlogStart = (link->backlogStart + 1) % SERVAL_LINK_LOG_BACKLOG;
	}
	link->backlog[slot] = line;
}
