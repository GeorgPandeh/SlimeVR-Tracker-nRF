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
#include "roam.h"

#include <string.h>

static void start_window(struct roam_link *link, uint32_t now) {
	link->sent_ok = 0;
	link->sent_failed = 0;
	link->window_start = now;
}

void roam_link_init(struct roam_link *link, uint32_t now) {
	memset(link, 0, sizeof(*link));
	link->window_start = now;
	link->cooldown_start = now;
	link->cooldown_ms = ROAM_COOLDOWN_MS;
}

void roam_link_connected(struct roam_link *link, uint32_t now) {
	start_window(link, now);
	link->bad_windows = 0;
}

void roam_link_record(struct roam_link *link, bool success) {
	if (success) {
		if (link->sent_ok != UINT16_MAX)
			link->sent_ok++;
	} else {
		if (link->sent_failed != UINT16_MAX)
			link->sent_failed++;
	}
}

bool roam_link_should_search(struct roam_link *link, uint32_t now) {
	if (now - link->window_start < ROAM_WINDOW_MS)
		return false;
	uint32_t ok = link->sent_ok;
	uint32_t failed = link->sent_failed;
	start_window(link, now);

	uint32_t attempts = ok + failed;
	if (attempts < ROAM_MIN_ATTEMPTS)
		return false;
	if (failed * 100 < attempts * ROAM_LOSS_PERCENT) {
		link->bad_windows = 0;
		return false;
	}
	if (link->bad_windows < UINT8_MAX)
		link->bad_windows++;
	if (link->bad_windows < ROAM_BAD_WINDOWS)
		return false;
	return now - link->cooldown_start >= link->cooldown_ms;
}

void roam_link_search_finished(struct roam_link *link, bool switched, uint32_t now) {
	link->cooldown_start = now;
	if (switched) {
		link->cooldown_ms = ROAM_COOLDOWN_MS;
	} else {
		// Nothing better is in range, so stop interrupting a weak link so often
		link->cooldown_ms *= 2;
		if (link->cooldown_ms > ROAM_COOLDOWN_MAX_MS)
			link->cooldown_ms = ROAM_COOLDOWN_MAX_MS;
	}
	link->bad_windows = 0;
}

void roam_link_connect_failed(struct roam_link *link, int slot, uint32_t now) {
	if (slot < 0 || slot >= ROAM_SLOTS)
		return;
	link->avoid_mask |= 1 << slot;
	link->avoid_start = now;
}

uint8_t roam_link_avoid_mask(struct roam_link *link, uint32_t now) {
	if (link->avoid_mask != 0 && now - link->avoid_start >= ROAM_AVOID_MS)
		link->avoid_mask = 0;
	return link->avoid_mask;
}

void roam_search_reset(struct roam_search *search) {
	memset(search, 0, sizeof(*search));
}

void roam_search_offer(struct roam_search *search, int slot, uint8_t channel, uint8_t rssi, int32_t timer_diff, uint32_t now) {
	if (slot < 0 || slot >= ROAM_SLOTS)
		return;
	if (!search->any_seen) {
		search->any_seen = true;
		search->first_seen = now;
	}
	struct roam_candidate *candidate = &search->candidates[slot];
	candidate->channel = channel;
	candidate->rssi = rssi;
	candidate->timer_diff = timer_diff;
	candidate->seen = true;
}

static int strongest_seen(const struct roam_search *search, uint8_t mask, int current_slot) {
	int best = ROAM_NO_SLOT;
	int best_score = 0;
	for (int slot = 0; slot < ROAM_SLOTS; slot++) {
		if (!(mask & (1 << slot)) || !search->candidates[slot].seen)
			continue;
		int score = search->candidates[slot].rssi;
		if (slot != current_slot)
			score += ROAM_RSSI_MARGIN;
		if (best == ROAM_NO_SLOT || score < best_score) {
			best = slot;
			best_score = score;
		}
	}
	return best;
}

int roam_search_pick(const struct roam_search *search, uint8_t paired_mask, uint8_t avoid_mask, int current_slot, uint32_t now) {
	if (!search->any_seen)
		return ROAM_NO_SLOT;

	uint8_t allowed = paired_mask & ~avoid_mask;
	bool all_seen = true;
	for (int slot = 0; slot < ROAM_SLOTS; slot++) {
		if ((allowed & (1 << slot)) && !search->candidates[slot].seen)
			all_seen = false;
	}
	if (!all_seen && now - search->first_seen < ROAM_SEARCH_WAIT_MS)
		return ROAM_NO_SLOT;

	int best = strongest_seen(search, allowed, current_slot);
	if (best == ROAM_NO_SLOT)
		best = strongest_seen(search, paired_mask, current_slot); // Better than no connection
	return best;
}
