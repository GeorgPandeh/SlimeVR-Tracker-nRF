
#include "globals.h"
#include "pairing.h"
#include "system/system.h"
#include "connection.h"
#include "system/clocks.h"
#include <zephyr/sys/crc.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(pairing, LOG_LEVEL_INF);

static struct pairing_discovery* discovered_dongles = NULL;
static struct pairing_discovery current_pairing_dongle;
static struct esb_payload tx_payload_pair = ESB_EMPTY_PAYLOAD(0, 16);
// Slot 0 is mirrored in retained->paired_addr, slot 1 is stored only in NVS
static uint8_t paired_addr[ROAM_SLOTS][8] = {0}; // Paired address bytes: <0> Is Paired | <1> Tracker Id | <2-8> Dongle Address
static int active_slot = 0;
static uint8_t slots_needing_save = 0;

static bool slot_is_paired(int slot) {
    return paired_addr[slot][0] != 0;
}

static void mark_slot_changed(int slot) {
    if(slot == 0)
        memcpy(retained->paired_addr, paired_addr[0], sizeof(paired_addr[0]));
    slots_needing_save |= 1 << slot;
}

static void use_slot(int slot) {
    active_slot = slot;
    retained->active_pair_slot = slot;
    connection_set_id(paired_addr[slot][1]);
    esb_set_addr_paired(&paired_addr[slot][2]);
}

void prepare_pair_payload() {
	uint64_t device_addr = *((uint64_t *) NRF_FICR->DEVICEADDR) & 0xFFFFFFFFFFFF;
	tx_payload_pair.pipe = 1;
	tx_payload_pair.noack = false;
	tx_payload_pair.data[0] = 0;
	tx_payload_pair.data[1] = ESB_PACKET_CONTROL_PAIR_REQEST;
	tx_payload_pair.data[2] = 0;
	memcpy(&tx_payload_pair.data[3], &device_addr, 6);
	memcpy(&tx_payload_pair.data[9], &device_addr, 6);
    tx_payload_pair.data[15] = ESB_DEVICE_TYPE_NORMAL;
}

void pairing_load_nvs(void) {
    sys_read(PAIRED_2_ID, paired_addr[1], sizeof(paired_addr[1]));
}

void pairing_restore(void) {
    // Read paired address from retained
	memcpy(paired_addr[0], retained->paired_addr, sizeof(paired_addr[0]));
    int slot = retained->active_pair_slot < ROAM_SLOTS ? retained->active_pair_slot : 0;
    if(!slot_is_paired(slot))
        slot = slot == 0 ? 1 : 0;
    if(slot_is_paired(slot)) {
        use_slot(slot);
        retained_update();
        esb_set_tracker_state(FIND_DONGLE);
    }
}

bool pairing_find_dongles_to_pair() {
    uint64_t start = k_uptime_get();
    esb_set_tracker_state(PAIRING_FIND_DONGLES);
    set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);
    // Find dongles around us
    esb_set_addr_discovery();
	clocks_allow_stopping(false);
	clocks_start();
    if(esb_initialize(false, true) != 0)
        return;
    if(discovered_dongles == NULL)
        discovered_dongles = k_calloc(sizeof(struct pairing_discovery), ESB_CHANNELS_AMOUNT);
    memset(discovered_dongles, 0, sizeof(struct pairing_discovery) * ESB_CHANNELS_AMOUNT);
    // TODO Move PAIRING_FIND_DONGLES to CONFIG
    // Wait for any dongle for up to 30 seconds, but continue if we find any in the first 2 seconds
    while(esb_get_tracker_state() == PAIRING_FIND_DONGLES && start + ESB_SEARCH_TIMEOUT > k_uptime_get()) {
        // Gather dongles for 2 seconds
        // TODO Move ESB_SEARCH_DONGLES_PAIRING to CONFIG
        if(esb_wait_state_change(PAIRING_FIND_DONGLES, ESB_SEARCH_DONGLES_PAIRING)) {
            return true; // State changed, it's not our problem anymore
        }
        for(int i = 0; i < ESB_CHANNELS_AMOUNT; ++i) {
            struct pairing_discovery* dg = &discovered_dongles[i];
            if(dg->dongle_hwid != 0 && (dg->flags & ESB_DONGLE_FLAG_ACCEPTS_NEW_TRACKERS) > 0) {
                esb_set_tracker_state(PAIRING_PICK_DONGLE);
                return true;
            }
        }
    }
	return false;
}

void pairing_dongle_found(const struct esb_payload *payload) {
    if(esb_get_tracker_state() == PAIRING_FIND_DONGLES && discovered_dongles != NULL) {
        uint64_t dongle_hwid = *((uint64_t *) &payload->data[2]) & 0xFFFFFFFFFFFF;
        uint8_t channel = payload->data[8];
        for(int i = 0; i < ESB_CHANNELS_AMOUNT; ++i) {
            struct pairing_discovery* dg = &discovered_dongles[i];
            if(dg->dongle_hwid == 0) {
                dg->dongle_hwid = dongle_hwid;
                dg->channel = channel;
                dg->rssi = payload->rssi;
                dg->flags = payload->data[9];
                dg->response = 255;
                dg->response_time = k_uptime_get_32();
                LOG_INF("New dongle for pairing: %012llX, ch %d, rssi %d, flags %d", dongle_hwid, channel, payload->rssi, payload->data[9]);
                break;
            } else if(dg->dongle_hwid == dongle_hwid) {
                dg->rssi = MIN(dg->rssi, payload->rssi);
                break;
            }
        }
    }
}

// May run from the ESB event handler, so flash is only written later in pairing_save_retained
void pairing_set_pair(uint64_t dongle_hwid, uint8_t tracker_id) {
    int slot = pairing_slot_for_dongle(dongle_hwid);
    if(slot == ROAM_NO_SLOT) {
        if(!slot_is_paired(0))
            slot = 0;
        else if(!slot_is_paired(1))
            slot = 1;
        else
            slot = active_slot == 0 ? 1 : 0; // Both in use: keep the dongle we are using
    }
	paired_addr[slot][0] = 1;
    paired_addr[slot][1] = tracker_id;
	memcpy(&paired_addr[slot][2], &dongle_hwid, 6);
    mark_slot_changed(slot);
	LOG_INF("Pairing info set in slot %d", slot);
    use_slot(slot);
    esb_set_tracker_state(FIND_DONGLE);
    set_led(SYS_LED_PATTERN_ONESHOT_COMPLETE, SYS_LED_PRIORITY_PAIR);
}

void pairing_save_retained() {
    uint8_t slots = slots_needing_save;
    if(slots != 0) {
        slots_needing_save = 0;
        if(slots & (1 << 0))
            sys_write(PAIRED_ID, retained->paired_addr, paired_addr[0], sizeof(paired_addr[0])); // Write new address and tracker id
        if(slots & (1 << 1))
            sys_write(PAIRED_2_ID, NULL, paired_addr[1], sizeof(paired_addr[1]));
        retained_update();
    }
}

uint64_t pairing_get_dongle(int slot) {
    uint64_t dongle_hwid = 0;
    memcpy(&dongle_hwid, &paired_addr[slot][2], 6);
    return dongle_hwid;
}

uint8_t pairing_get_tracker_id(int slot) {
    return paired_addr[slot][1];
}

uint64_t pairing_get_paired_dongle() {
    return pairing_get_dongle(active_slot);
}

int pairing_slot_for_dongle(uint64_t dongle_hwid) {
    for(int slot = 0; slot < ROAM_SLOTS; ++slot) {
        if(slot_is_paired(slot) && pairing_get_dongle(slot) == dongle_hwid)
            return slot;
    }
    return ROAM_NO_SLOT;
}

int pairing_active_slot(void) {
    return active_slot;
}

uint8_t pairing_paired_mask(void) {
    uint8_t mask = 0;
    for(int slot = 0; slot < ROAM_SLOTS; ++slot) {
        if(slot_is_paired(slot))
            mask |= 1 << slot;
    }
    return mask;
}

bool pairing_has_alternate(void) {
    return pairing_paired_mask() == ((1 << ROAM_SLOTS) - 1);
}

void pairing_select_slot(int slot) {
    use_slot(slot);
}

void pairing_dongle_response(const struct esb_payload *payload) {
    if(payload->length < 15) {
        LOG_ERR("Response payload too short");
        return;
    }
    uint64_t device_addr = *((uint64_t *) NRF_FICR->DEVICEADDR) & 0xFFFFFFFFFFFF;
    uint64_t dongle_hwid = *((uint64_t *) &payload->data[3]) & 0xFFFFFFFFFFFF;
    uint64_t tracker_hwid = *((uint64_t *) &payload->data[9]) & 0xFFFFFFFFFFFF;
    uint8_t tracker_id = payload->data[2];
    LOG_INF("Dongle reponse: %012llX, %012llX, %d", dongle_hwid, tracker_hwid, tracker_id);
    if(esb_get_tracker_state() == PAIRING_PICK_DONGLE && current_pairing_dongle.dongle_hwid == dongle_hwid && tracker_hwid == device_addr) {
        if(tracker_id >= 200) { // Rejected
            LOG_INF("Pairing rejected from dongle %012llX, reason: %d", dongle_hwid, tracker_id);
            esb_set_tracker_state(PAIRING_REJECTED);
        } else {
            LOG_INF("Pairing accepted from dongle %012llX, tracker id: %d", dongle_hwid, tracker_id);
            pairing_set_pair(dongle_hwid, tracker_id);
            esb_set_tracker_state(FIND_DONGLE);
        }
        // TODO : Add WAIT_RESPONSE state on specific error codes
        // to confirm from the user that we need to pair
    }
}

int compare_dongles(const void* a, const void* b) {
    const struct pairing_discovery* a_d = (const struct pairing_discovery*) a;
    const struct pairing_discovery* b_d = (const struct pairing_discovery*) b;
    int flag_comp = (a_d->flags & ESB_DONGLE_FLAG_ACCEPTS_NEW_TRACKERS) - (b_d->flags & ESB_DONGLE_FLAG_ACCEPTS_NEW_TRACKERS);
    if(flag_comp != 0)
        return flag_comp;
    flag_comp = (a_d->flags & ESB_DONGLE_FLAG_FORCE_PAIRING) - (b_d->flags & ESB_DONGLE_FLAG_FORCE_PAIRING);
    if(flag_comp != 0)
        return flag_comp;
    // Try dongles we don't know yet first, so that pairing again adds the second dongle
    flag_comp = (pairing_slot_for_dongle(a_d->dongle_hwid) != ROAM_NO_SLOT) - (pairing_slot_for_dongle(b_d->dongle_hwid) != ROAM_NO_SLOT);
    if(flag_comp != 0)
        return flag_comp;
    return (a_d->rssi > b_d->rssi) - (a_d->rssi < b_d->rssi);
}

bool pairing_pick_dongle_and_pair(void) {
    set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);
    int members = 0;
	for(; members < ESB_CHANNELS_AMOUNT; ++members) {
        struct pairing_discovery* dg = &discovered_dongles[members];
        if(dg->dongle_hwid == 0) {
            break;
        }
    }
    if(members != 0) {
        qsort(discovered_dongles, members, sizeof(struct pairing_discovery), compare_dongles);
    }
    for(int i = 0; i < members; ++i) {
        struct pairing_discovery* dg = &discovered_dongles[i];
        if(dg->dongle_hwid != 0) {
            LOG_INF("Found dongle %d: %012llX, signal %d, flags %x", i, dg->dongle_hwid, dg->rssi, dg->flags);
        }
    }
    for(int i = 0; i < members; ++i) {
        struct pairing_discovery* dg = &discovered_dongles[i];
        if(dg->dongle_hwid != 0 && (dg->flags & ESB_DONGLE_FLAG_ACCEPTS_NEW_TRACKERS) > 0) {
            LOG_INF("Trying to pair to %012llX...", dg->dongle_hwid);
            current_pairing_dongle = *dg;
            esb_set_tracker_state(PAIRING_PICK_DONGLE);
            prepare_pair_payload();
            esb_set_channel(current_pairing_dongle.channel);
            esb_set_receiver_addr(current_pairing_dongle.dongle_hwid);
            if(esb_initialize(true, false) != 0)
                return false;
            uint32_t start = k_uptime_get_32();
            while(start + 2000 > k_uptime_get_32() && esb_get_tracker_state() == PAIRING_PICK_DONGLE) {
                // TODO If we use channel hopping, we need to do something with timings here
                // We should sync our timer to the received packets
                int ret = esb_write_payload(&tx_payload_pair);
                if(ret < 0) {
                    LOG_ERR("Write error: %d", ret);
                    return false;
                }
                ret = esb_start_tx();
                if(ret < 0) {
                    LOG_ERR("TX error: %d", ret);
                    return false;
                }
                k_msleep(20);
            }
            // Wait up to 5 minutes for the user to approve the request
            if(!esb_wait_state_change(PAIRING_WAIT_RESPONSE, 300000)) {
                esb_set_tracker_state(PAIRING_REJECTED);
            }
            switch(esb_get_tracker_state()) {
                case PAIRING_REJECTED:
                case PAIRING_PICK_DONGLE:
                    break; // Continue with the next dongle
                case FIND_DONGLE:
                case DONGLE_CONNECT:
                    // Yooo??? dongle?? said "yes"???
                    // By this time we already have everything saved, we go directly to finding it
                    return true;
                default:
                    return true; // State was changed outside, it's not our problem anymore
            }
        }
    }
	return false; // No dongle was found and we're still in the state of finding it
}

void pairing_request_pair(void)
{
    esb_set_tracker_state(PAIRING_FIND_DONGLES);
    LOG_INF("Pairing requested, looking for dongle");
}

void pairing_clear_pair(void)
{
    memset(paired_addr, 0, sizeof(paired_addr));
    active_slot = 0;
    retained->active_pair_slot = 0;
	sys_write(PAIRED_ID, &retained->paired_addr, paired_addr[0], sizeof(paired_addr[0])); // write zeroes
	sys_write(PAIRED_2_ID, NULL, paired_addr[1], sizeof(paired_addr[1]));
	LOG_INF("Pairing data reset");
	pairing_request_pair();
}

// May run from the ESB event handler, so flash is only written later in pairing_save_retained
void pairing_forget_active(void)
{
    int forgotten = active_slot;
    int other = forgotten == 0 ? 1 : 0;
    memset(paired_addr[forgotten], 0, sizeof(paired_addr[forgotten]));
    mark_slot_changed(forgotten);
    LOG_INF("Pairing in slot %d forgotten", forgotten);
    if(slot_is_paired(other)) {
        use_slot(other);
        esb_set_tracker_state(FIND_DONGLE);
    } else {
        pairing_request_pair();
    }
}