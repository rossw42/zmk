/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * SKELETON — not yet wired into app/src/studio/CMakeLists.txt.
 *
 * ZMK Studio RPC subsystem for editing combos at runtime. Depends on:
 *   1. combos.proto being added to zmk-studio-messages (see ../proto/), which generates the
 *      zmk_combos_* nanopb types and the zmk_studio_Request_combos_tag used by ZMK_RPC_SUBSYSTEM.
 *   2. The runtime combo API declared in <zmk/combos.h> (see combos.h next to this file) being
 *      implemented in app/src/combo.c behind CONFIG_ZMK_COMBOS_SETTINGS_STORAGE.
 *
 * The shape deliberately mirrors keymap_subsystem.c: streaming nanopb encoders via pb_callback so
 * no full response is ever buffered in RAM, errno -> proto error code mapping per handler, and an
 * "unsaved changes" notification raised on every successful mutation.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/combos.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)
#define COMBOS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(combos, type, __VA_ARGS__)

/* --------------------------------------------------------------------------------------------- */
/* Encoding helpers                                                                              */
/* --------------------------------------------------------------------------------------------- */

static void populate_combo_msg(zmk_combos_Combo *msg, const struct zmk_combo *combo) {
    // combos.options.in gives key_positions a `max_count`, so nanopb emits it as a fixed array plus
    // a `_count` member (not a pb_callback_t). Copy directly; no streaming callback needed.
    msg->key_positions_count = MIN(combo->key_positions_len, ARRAY_SIZE(msg->key_positions));
    for (size_t i = 0; i < msg->key_positions_count; i++) {
        msg->key_positions[i] = combo->key_positions[i];
    }

    msg->has_binding = true;
    msg->binding.behavior_id = zmk_behavior_get_local_id(combo->binding.behavior_dev);
    msg->binding.param1 = combo->binding.param1;
    msg->binding.param2 = combo->binding.param2;

    msg->timeout_ms = combo->timeout_ms;
    msg->require_prior_idle_ms =
        combo->require_prior_idle_ms > 0 ? (uint32_t)combo->require_prior_idle_ms : 0;
    msg->layer_mask = combo->layer_mask;
    msg->slow_release = combo->slow_release;
}

static void populate_slot_msg(zmk_combos_ComboSlot *msg, size_t index) {
    const struct zmk_combo *combo = zmk_combos_get(index);

    msg->index = index;
    msg->stock = zmk_combos_slot_is_stock(index);
    msg->populated = combo != NULL;

    if (combo) {
        msg->has_combo = true;
        populate_combo_msg(&msg->combo, combo);
    }
}

static bool encode_combo_slots(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const size_t slot_count = zmk_combos_slot_count();

    for (size_t i = 0; i < slot_count; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_combos_ComboSlot slot = zmk_combos_ComboSlot_init_zero;
        populate_slot_msg(&slot, i);

        if (!pb_encode_submessage(stream, &zmk_combos_ComboSlot_msg, &slot)) {
            LOG_WRN("Failed to encode combo slot %d", i);
            return false;
        }
    }

    return true;
}

/* --------------------------------------------------------------------------------------------- */
/* Handlers                                                                                      */
/* --------------------------------------------------------------------------------------------- */

zmk_studio_Response get_combos(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_combos_GetCombosResponse resp = zmk_combos_GetCombosResponse_init_zero;

    resp.slots.funcs.encode = encode_combo_slots;
    resp.max_keys_per_combo = zmk_combos_max_keys_per_combo();
    resp.available_slots = zmk_combos_available_slots();

    return COMBOS_RESPONSE(get_combos, resp);
}

static zmk_combos_SetComboErrorCode map_set_errno(int err) {
    switch (err) {
    case -EINVAL:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_INDEX;
    case -E2BIG:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_TOO_MANY_KEYS;
    case -ENODATA:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_TOO_FEW_KEYS;
    case -ERANGE:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_KEY_POSITION;
    case -EEXIST:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_DUPLICATE_KEY_POSITION;
    case -ENODEV:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BEHAVIOR;
    case -EDOM:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_PARAMETERS;
    default:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
    }
}

zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;
    zmk_combos_SetComboResponse resp = zmk_combos_SetComboResponse_init_zero;

    if (!set_req->has_combo || !set_req->combo.has_binding) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
        return COMBOS_RESPONSE(set_combo, resp);
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(set_req->combo.binding.behavior_id);
    if (!behavior_name) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BEHAVIOR;
        return COMBOS_RESPONSE(set_combo, resp);
    }

    struct zmk_combo combo = {
        .binding =
            {
                .behavior_dev = behavior_name,
                .param1 = set_req->combo.binding.param1,
                .param2 = set_req->combo.binding.param2,
            },
        .timeout_ms = set_req->combo.timeout_ms,
        .require_prior_idle_ms = set_req->combo.require_prior_idle_ms,
        .layer_mask = set_req->combo.layer_mask,
        .slow_release = set_req->combo.slow_release,
        .key_positions_len = set_req->combo.key_positions_count,
    };

    // With `max_count` set in combos.options.in, nanopb decodes key_positions into a fixed array
    // and has already rejected anything longer, so this copy is bounded.
    for (size_t i = 0; i < combo.key_positions_len && i < ARRAY_SIZE(combo.key_positions); i++) {
        combo.key_positions[i] = set_req->combo.key_positions[i];
    }

    if (zmk_behavior_validate_binding(&combo.binding) < 0) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_PARAMETERS;
        return COMBOS_RESPONSE(set_combo, resp);
    }

    int ret = zmk_combos_set(set_req->index, &combo);
    if (ret < 0) {
        LOG_WRN("Failed to set combo %d: %d", set_req->index, ret);
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = map_set_errno(ret);
        return COMBOS_RESPONSE(set_combo, resp);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, true)});

    resp.which_result = zmk_combos_SetComboResponse_ok_tag;
    populate_slot_msg(&resp.result.ok, set_req->index);

    return COMBOS_RESPONSE(set_combo, resp);
}

zmk_studio_Response clear_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_ClearComboRequest *clear_req = &req->subsystem.combos.request_type.clear_combo;
    zmk_combos_ClearComboResponse resp = zmk_combos_ClearComboResponse_init_zero;

    int ret = zmk_combos_clear(clear_req->index);
    if (ret < 0) {
        LOG_WRN("Failed to clear combo %d: %d", clear_req->index, ret);
        resp.which_result = zmk_combos_ClearComboResponse_err_tag;
        switch (ret) {
        case -EINVAL:
            resp.result.err = zmk_combos_ClearComboErrorCode_CLEAR_COMBO_ERR_INVALID_INDEX;
            break;
        case -EPERM:
            resp.result.err = zmk_combos_ClearComboErrorCode_CLEAR_COMBO_ERR_STOCK_COMBO;
            break;
        default:
            resp.result.err = zmk_combos_ClearComboErrorCode_CLEAR_COMBO_ERR_GENERIC;
            break;
        }
        return COMBOS_RESPONSE(clear_combo, resp);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, true)});

    resp.which_result = zmk_combos_ClearComboResponse_ok_tag;
    resp.result.ok = true;

    return COMBOS_RESPONSE(clear_combo, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combos, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, clear_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* --------------------------------------------------------------------------------------------- */
/* Save / discard / reset integration                                                            */
/* --------------------------------------------------------------------------------------------- */

// There is intentionally no combos-specific save/discard RPC. The existing keymap
// save_changes / discard_changes / check_unsaved_changes handlers are extended to also call
// zmk_combos_save_changes() / zmk_combos_discard_changes() / zmk_combos_check_unsaved_changes(),
// so a single "Save" in the UI persists everything. Settings reset uses the same hook as keymap.

static int combos_settings_reset(void) { return zmk_combos_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(combos, combos_settings_reset);

// This subsystem raises its notifications directly; see keymap_subsystem.c for why a mapper that
// declines everything must return a negative value.
static int combos_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    return -ENOTSUP;
}

ZMK_RPC_EVENT_MAPPER(combos, combos_event_mapper);
