/* Copyright (c) 2026 Steven Gann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SERVAL_LINK_H
#define GBA_SERVAL_LINK_H

#include <mgba-util/common.h>

CXX_GUARD_START

// Serval Engine's debug link: the two registers its agent uses to find the
// endpoint (Serval Engine's docs/debug-link.md, "Finding the endpoint"), in a
// range real hardware ignores, next to mGBA's debug registers.
//
// 0x04FFF800 presence (16 bits): the agent writes 0x5E41 and reads back
//            0x1A5E when an endpoint is installed.
// 0x04FFF804 doorbell (32 bits): the agent writes its mailbox's address; the
//            endpoint handles the ring during the write.
//
// Nothing changes unless a frontend installs a hook (GBA.servalLink): stock
// behaviour (open bus, stub writes) otherwise.

enum {
	GBA_REG_SERVAL_LINK_PRESENCE = 0xFFF800,
	GBA_REG_SERVAL_LINK_DOORBELL = 0xFFF804,

	GBA_SERVAL_LINK_ASK = 0x5E41,
	GBA_SERVAL_LINK_ANSWER = 0x1A5E,
};

struct GBA;

struct GBAServalLink {
	// Called during the doorbell's write, with the value written (the
	// mailbox's address). It may call GBAInterrupt() to stop the run loop
	// right after the storing instruction.
	void (*ring)(struct GBAServalLink*, struct GBA*, uint32_t mailbox);
	// Set by a write of GBA_SERVAL_LINK_ASK to the presence register.
	bool asked;
};

CXX_GUARD_END

#endif
