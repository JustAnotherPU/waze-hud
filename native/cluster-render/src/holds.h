/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The guide hold's settings (cluster-render.conf "guide hold", iDrive 7):
 * unless guide_hold = 0, a hold (slots.h) per slot of guide_slots that
 * follows the map, appended to the targets; the mapping guard disables a
 * hold at its second strike (guard.h). */
#ifndef CRS_HOLDS_H
#define CRS_HOLDS_H

#include <stdint.h>
#include <string>
#include <vector>

#include "slots.h"

namespace crs {

/* The settings as written. */
struct HoldSettings {
    std::string hold, slots, scene_id, content_class, take_over;   /* guide_hold, guide_slots, ... */
};

/* Appends a hold per slot of slots unless hold is 0; hold other than 0 or 1
 * counts as 1, with *warn saying so. slots: hex slot ids 0x1-0xfff,
 * separated by commas or spaces, each once and no other target's. Scene ids
 * from scene_id up, one per slot, each in allowed and no other target's.
 * take_over: the hex content ids the holds' take-overs may unmap, at least
 * one, separated the same way, never 0x01000314 (the seat-belt-warning
 * panel). Anything else adds no hold: false, with *warn naming it, and
 * targets left as they were. */
bool load_holds(const HoldSettings &cfg, const std::vector<uint64_t> &allowed, std::vector<TargetSpec> *targets,
                std::string *warn);

}  // namespace crs

#endif
