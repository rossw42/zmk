/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * PROPOSED public header: app/include/zmk/combos.h
 *
 * Runtime combo API consumed by the ZMK Studio `combos` RPC subsystem. Implemented in
 * app/src/combo.c behind CONFIG_ZMK_COMBOS_SETTINGS_STORAGE (selected by ZMK_STUDIO_RPC), mirroring
 * how CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE turns `zmk_keymap` from `const` into a mutable array with
 * a `const zmk_stock_keymap` shadow and a pending-changes bitmap.
 *
 * Slot model
 * ----------
 * `combos[]` keeps its compile-time size. Every child of the `zmk,combos` node becomes a slot:
 *   - status "okay"     -> a *stock* slot, populated from DT, editable, cannot be cleared.
 *   - status "reserved" -> an *empty* slot Studio can fill / edit / clear. A reserved combo must
 *                          still declare `key-positions` (values are ignored) because its length
 *                          participates in the MAX_COMBO_KEYS sizeof-union, which is how the user
 *                          sizes runtime combos, e.g.:
 *
 *       studio_combo_a { status = "reserved"; key-positions = <0 0 0 0>; bindings = <&none>; };
 *
 * Because the matcher relies on `combos[]` being sorted shortest-first (see COMBO_INST in
 * combo.c), the implementation keeps a separate `uint16_t order[]` index sorted by
 * key_positions_len and iterates through it instead of walking `combos[]` directly. Rebuilding that
 * order and `combo_lookup` on every mutation is O(n log n) for n <= a few dozen; negligible.
 *
 * Persistence
 * -----------
 * Settings key "combos/<slot>" holds a packed struct
 *   { zmk_behavior_local_id_t behavior_local_id; uint32_t param1, param2;
 *     uint16_t timeout_ms; int16_t require_prior_idle_ms; uint32_t layer_mask;
 *     uint8_t flags; uint8_t key_positions_len; uint8_t key_positions[]; }
 * so a 4-key combo is ~24 bytes. A cleared reserved slot deletes its key. Stock slots that are
 * reverted to their DT values also delete their key so flash isn't wasted on no-op overrides.
 *
 * Position semantics
 * ------------------
 * `key_positions` are stock-layout positions, identical to devicetree. Translation to the selected
 * physical layout happens in combo.c when the lookup is rebuilt (fix for zmkfirmware/zmk#3228), so
 * the RPC layer never has to think about position maps.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

/* Set by combo.c from the sizeof-union over all DT combos (okay + reserved). */
#ifndef ZMK_COMBOS_MAX_KEYS
#define ZMK_COMBOS_MAX_KEYS CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO_STUDIO
#endif

struct zmk_combo {
    uint32_t key_positions[ZMK_COMBOS_MAX_KEYS];
    uint8_t key_positions_len;

    struct zmk_behavior_binding binding;

    uint32_t timeout_ms;           /* 0 => default (50) */
    int32_t require_prior_idle_ms; /* <= 0 => disabled */
    uint32_t layer_mask;           /* 0 => all layers */
    bool slow_release;
};

/** @brief Total number of slots (stock + reserved), fixed at build time. */
size_t zmk_combos_slot_count(void);

/** @brief Longest key_positions a slot can hold, fixed at build time. */
size_t zmk_combos_max_keys_per_combo(void);

/** @brief Number of reserved slots currently unpopulated. */
size_t zmk_combos_available_slots(void);

/** @brief True if the slot originates from a status="okay" devicetree combo. */
bool zmk_combos_slot_is_stock(size_t index);

/**
 * @brief Current (possibly unsaved) contents of a slot.
 * @retval NULL if the index is invalid or the slot is an empty reserved slot.
 */
const struct zmk_combo *zmk_combos_get(size_t index);

/**
 * @brief Replace the contents of a slot and rebuild the matcher tables.
 *
 * @retval 0 on success
 * @retval -EINVAL  invalid index
 * @retval -ENODATA fewer than 2 key positions
 * @retval -E2BIG   more than zmk_combos_max_keys_per_combo() key positions
 * @retval -ERANGE  a key position is >= ZMK_KEYMAP_LEN
 * @retval -EEXIST  a key position is repeated within the combo
 * @retval -ENODEV  binding.behavior_dev does not name a known behavior
 * @retval -EDOM    binding parameters rejected by zmk_behavior_validate_binding()
 */
int zmk_combos_set(size_t index, const struct zmk_combo *combo);

/**
 * @brief Empty a reserved slot and rebuild the matcher tables.
 *
 * @retval 0 on success
 * @retval -EINVAL invalid index
 * @retval -EPERM  the slot is a stock combo (edit it instead)
 */
int zmk_combos_clear(size_t index);

/* Save / discard follow the exact semantics of the zmk_keymap_* equivalents. */
int zmk_combos_check_unsaved_changes(void);
int zmk_combos_save_changes(void);
int zmk_combos_discard_changes(void);
int zmk_combos_reset_settings(void);
