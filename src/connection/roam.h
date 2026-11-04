/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2026 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
/*
 * Decisions for switching between two paired dongles. This file only holds the
 * logic, with no radio or OS calls, so it can be tested on a host machine.
 *
 * Stay on a dongle until the link gets bad, then listen for both dongles and
 * connect to the stronger one. A tracker with one paired dongle never switches.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROAM_SLOTS 2
#define ROAM_NO_SLOT (-1)

// Link quality is judged over consecutive windows of this length
#define ROAM_WINDOW_MS 200
// Windows with fewer sends than this say nothing about the link (e.g. tracker is idle)
#define ROAM_MIN_ATTEMPTS 5
// A window is bad when at least this share of sends failed
#define ROAM_LOSS_PERCENT 50
// Consecutive bad windows needed to start looking for a better dongle
#define ROAM_BAD_WINDOWS 3
// Minimum time between searches. Doubles each time a search ends on the same dongle.
#define ROAM_COOLDOWN_MS 5000
#define ROAM_COOLDOWN_MAX_MS 30000
// After hearing one paired dongle, how long to wait for the other. Dongles
// advertise about once per second, so this must be longer than that.
#define ROAM_SEARCH_WAIT_MS 1200
// The other dongle must be this much stronger (dB) to be chosen over the current one
#define ROAM_RSSI_MARGIN 6
// How long to avoid a dongle that was heard but did not answer our connect attempts.
// A dongle with an amplifier can be heard from further away than it can hear us.
#define ROAM_AVOID_MS 30000

struct roam_link {
	uint16_t sent_ok;
	uint16_t sent_failed;
	uint8_t bad_windows;
	uint8_t avoid_mask;
	uint32_t window_start;
	uint32_t cooldown_start;
	uint32_t cooldown_ms;
	uint32_t avoid_start;
};

struct roam_candidate {
	bool seen;
	uint8_t channel;
	uint8_t rssi; // Magnitude as reported by the radio: lower is stronger
	int32_t timer_diff;
};

struct roam_search {
	struct roam_candidate candidates[ROAM_SLOTS];
	bool any_seen;
	uint32_t first_seen;
};

void roam_link_init(struct roam_link *link, uint32_t now);
// Start judging a new connection, keeping any running cooldown
void roam_link_connected(struct roam_link *link, uint32_t now);
void roam_link_record(struct roam_link *link, bool success);
// Call regularly while connected. True when it is time to search for a better dongle.
bool roam_link_should_search(struct roam_link *link, uint32_t now);
void roam_link_search_finished(struct roam_link *link, bool switched, uint32_t now);
void roam_link_connect_failed(struct roam_link *link, int slot, uint32_t now);
// Slots to avoid right now, as a mask with bit N set for slot N
uint8_t roam_link_avoid_mask(struct roam_link *link, uint32_t now);

void roam_search_reset(struct roam_search *search);
void roam_search_offer(struct roam_search *search, int slot, uint8_t channel, uint8_t rssi, int32_t timer_diff, uint32_t now);
// Returns the slot to connect to, or ROAM_NO_SLOT to keep listening.
// paired_mask has bit N set when slot N holds a paired dongle. A dongle in
// avoid_mask is only used when no other paired dongle has been heard.
int roam_search_pick(const struct roam_search *search, uint8_t paired_mask, uint8_t avoid_mask, int current_slot, uint32_t now);
