#pragma once

#include "esb.h"
#include "roam.h"

// Read the pairing that is not kept in retained memory
void pairing_load_nvs(void);
void pairing_restore(void);
void pairing_dongle_found(const struct esb_payload *payload);
void pairing_dongle_response(const struct esb_payload *payload);
bool pairing_pick_dongle_and_pair(void);
void pairing_request_pair(void);
void pairing_clear_pair(void);
bool pairing_find_dongles_to_pair(void);
void pairing_set_pair(uint64_t dongle_hwid, uint8_t tracker_id);
void pairing_save_retained();
uint64_t pairing_get_paired_dongle();
// Forget only the dongle in use, for when that dongle no longer accepts this tracker
void pairing_forget_active(void);
int pairing_slot_for_dongle(uint64_t dongle_hwid);
int pairing_active_slot(void);
// Bit N is set when slot N holds a paired dongle
uint8_t pairing_paired_mask(void);
bool pairing_has_alternate(void);
// Point the tracker's address and ID at the dongle in this slot
void pairing_select_slot(int slot);
uint64_t pairing_get_dongle(int slot);
uint8_t pairing_get_tracker_id(int slot);