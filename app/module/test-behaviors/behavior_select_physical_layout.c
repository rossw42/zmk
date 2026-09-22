/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_select_physical_layout

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/physical_layouts.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_select_physical_layout_binding_pressed(struct zmk_behavior_binding *binding,
                                                     struct zmk_behavior_binding_event event) {
    int result = zmk_physical_layouts_select(binding->param1);

    if (result < 0) {
        LOG_ERR("Failed to select physical layout %d (err: %d)", binding->param1, result);
        return result;
    }

    LOG_DBG("Selected physical layout %d", binding->param1);
    return 0;
}

static int on_select_physical_layout_binding_released(struct zmk_behavior_binding *binding,
                                                      struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_select_physical_layout_driver_api = {
    .binding_pressed = on_select_physical_layout_binding_pressed,
    .binding_released = on_select_physical_layout_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_select_physical_layout_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
