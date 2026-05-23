static int desktop_any_module_app_open(void) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        if (module_app_windows[i].open) {
            return 1;
        }
    }
    return 0;
}

static int desktop_any_module_app_minimized(void) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        if (module_app_windows[i].minimized) {
            return 1;
        }
    }
    return 0;
}

static desktop_module_window_t *desktop_find_module_app_window(uint32_t index) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        desktop_module_window_t *slot = &module_app_windows[i];
        int slot_index = desktop_module_index_for_slot(slot);
        if ((slot->open || slot->minimized) && slot_index >= 0 && (uint32_t)slot_index == index) {
            return slot;
        }
    }
    return 0;
}

static desktop_module_window_t *desktop_allocate_module_app_window(void) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        desktop_module_window_t *slot = &module_app_windows[i];
        if (!slot->open && !slot->minimized) {
            return slot;
        }
    }
    return 0;
}

static desktop_module_window_t *desktop_top_module_app_window_at(uint32_t x, uint32_t y) {
    for (uint32_t i = DESKTOP_MODULE_APP_WINDOWS_MAX; i > 0u; --i) {
        desktop_module_window_t *slot = &module_app_windows[i - 1u];
        if (slot->open &&
            point_in_rect(x, y, slot->window.x, slot->window.y, slot->window.w, slot->window.h)) {
            return slot;
        }
    }
    return 0;
}

static void desktop_focus_module_app(desktop_module_window_t *slot) {
    if (slot != 0 && slot->open) {
        module_focused = slot;
    }
}

static void desktop_blur_module_app(void) {
    module_focused = 0;
}

static int desktop_module_app_send_key(desktop_module_window_t *slot, const key_event_t *key) {
    int rc;
    int module_index;

    module_index = desktop_module_index_for_slot(slot);
    if (slot == 0 || key == 0 || !slot->open || module_index < 0) {
        return 0;
    }

    graphics_viewport_push(slot->window.content_x,
                           slot->window.content_y,
                           slot->window.content_w,
                           slot->window.content_h);
    rc = shell_module_key((uint32_t)module_index, (uint32_t)key->type, (uint32_t)(uint8_t)key->ch);
    graphics_viewport_pop();
    if (rc == 0) {
        return 1;
    }
    return 0;
}

static int desktop_module_app_send_mouse(desktop_module_window_t *slot,
                                         uint32_t x,
                                         uint32_t y,
                                         uint32_t buttons,
                                         int32_t wheel) {
    int rc;
    int module_index;

    module_index = desktop_module_index_for_slot(slot);
    if (slot == 0 || !slot->open || module_index < 0) {
        return 0;
    }
    if (!point_in_rect(x,
                       y,
                       slot->window.content_x,
                       slot->window.content_y,
                       slot->window.content_w,
                       slot->window.content_h)) {
        return 0;
    }

    x -= slot->window.content_x;
    y -= slot->window.content_y;
    graphics_viewport_push(slot->window.content_x,
                           slot->window.content_y,
                           slot->window.content_w,
                           slot->window.content_h);
    rc = shell_module_mouse((uint32_t)module_index, x, y, buttons, wheel);
    graphics_viewport_pop();
    if (rc == 0) {
        if (wheel == 0) {
            desktop_damage_window(&slot->window);
        }
        return 1;
    }
    return 0;
}

static void desktop_draw_modules(void) {
    uint32_t ui_count;

    if (!modules_open) {
        return;
    }
    if (!desktop_damage_intersects_rect(modules_window.x,
                                        modules_window.y,
                                        modules_window.w + 6u,
                                        modules_window.h + 6u)) {
        return;
    }

    desktop_draw_window(&modules_window, "Modules");
    ui_count = desktop_app_catalog_count();
    if (ui_count == 0u) {
        console_draw_text_at_pixel(modules_window.content_x + 8u, modules_window.content_y + 8u, "No apps in /mods", 0xf5fbf7u, 0x222c34u);
        return;
    }

    for (uint32_t i = 0; i < ui_count; ++i) {
        char name[DESKTOP_MODULE_NAME_SIZE];
        uint32_t row_y = modules_window.content_y + 8u + i * 26u;
        if (row_y + 24u >= modules_window.y + modules_window.h) {
            break;
        }
        if (desktop_app_catalog_name_at(i, name, sizeof(name)) != 0) {
            text_copy_limited(name, sizeof(name), "module");
        }
        desktop_draw_button(modules_window.content_x + 4u, row_y, modules_window.content_w - 8u, name, 0);
    }
}

static int desktop_module_app_tick_due(const desktop_module_window_t *slot, int force, unsigned long long now) {
    if (desktop_module_index_for_slot((desktop_module_window_t *)slot) < 0) {
        return 0;
    }

    unsigned int hz = timer_frequency();
    unsigned long long interval;

    if (hz == 0u) {
        hz = 100u;
    }
    interval = hz / DESKTOP_MODULE_TICK_HZ;
    if (interval == 0ull) {
        interval = 1ull;
    }

    if (!force && start_menu_open) {
        return 0;
    }
    if (!force && now - slot->last_tick < interval) {
        return 0;
    }

    return 1;
}

static int desktop_module_content_occluded(const desktop_module_window_t *slot) {
    uint32_t menu_x;
    uint32_t menu_y;
    uint32_t menu_w;
    uint32_t menu_h;
    uint32_t first;

    if (slot == 0 ||
        slot < module_app_windows ||
        slot >= module_app_windows + DESKTOP_MODULE_APP_WINDOWS_MAX) {
        return 1;
    }

    if (editor_open &&
        desktop_rects_intersect(slot->window.content_x,
                                slot->window.content_y,
                                slot->window.content_w,
                                slot->window.content_h,
                                editor_window.x,
                                editor_window.y,
                                editor_window.w,
                                editor_window.h)) {
        return 1;
    }

    if (start_menu_open) {
        desktop_start_menu_rect(&menu_x, &menu_y, &menu_w, &menu_h);
        if (desktop_rects_intersect(slot->window.content_x,
                                    slot->window.content_y,
                                    slot->window.content_w,
                                    slot->window.content_h,
                                    menu_x,
                                    menu_y,
                                    menu_w,
                                    menu_h)) {
            return 1;
        }
    }

    first = (uint32_t)(slot - module_app_windows) + 1u;
    for (uint32_t i = first; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        if (module_app_windows[i].open &&
            desktop_rects_intersect(slot->window.content_x,
                                    slot->window.content_y,
                                    slot->window.content_w,
                                    slot->window.content_h,
                                    module_app_windows[i].window.x,
                                    module_app_windows[i].window.y,
                                    module_app_windows[i].window.w,
                                    module_app_windows[i].window.h)) {
            return 1;
        }
    }

    return 0;
}

static int desktop_tick_module_app(desktop_module_window_t *slot, int force) {
    unsigned long long now = timer_ticks();
    int module_index;

    if (!desktop_module_app_tick_due(slot, force, now)) {
        return 0;
    }
    slot->last_tick = now;
    module_index = desktop_module_index_for_slot(slot);
    if (module_index < 0) {
        return 0;
    }

    if (!desktop_module_content_occluded(slot)) {
        desktop_begin_paint();
        graphics_viewport_push(slot->window.content_x,
                               slot->window.content_y,
                               slot->window.content_w,
                               slot->window.content_h);
        console_cursor_enable(0);
        console_set_output_hook(0);
        console_reset_region();
        (void)shell_module_tick((uint32_t)module_index);
        graphics_viewport_pop();
    }

    if (desktop_deferred_redraw) {
        desktop_deferred_redraw = 0;
        if (desktop_damage_count == 0u) {
            desktop_damage_full();
        }
        desktop_redraw_all();
    }
    return 1;
}

static void desktop_draw_module_app(void) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        desktop_module_window_t *slot = &module_app_windows[i];
        const char *name;
        int module_index;

        if (!slot->open) {
            continue;
        }

        module_index = desktop_module_index_for_slot(slot);
        name = module_index < 0 ? 0 : shell_module_name((uint32_t)module_index);
        if (name == 0) {
            slot->open = 0;
            slot->minimized = 0;
            slot->state.maximized = 0;
            slot->unload_on_close = 0;
            slot->module_name[0] = '\0';
            continue;
        }
        if (!desktop_damage_intersects_rect(slot->window.x,
                                            slot->window.y,
                                            slot->window.w + 6u,
                                            slot->window.h + 6u)) {
            continue;
        }

        desktop_draw_window(&slot->window, name);
        graphics_fill_rect(slot->window.content_x,
                           slot->window.content_y,
                           slot->window.content_w,
                           slot->window.content_h,
                           0x101820u);
        graphics_viewport_push(slot->window.content_x,
                               slot->window.content_y,
                               slot->window.content_w,
                               slot->window.content_h);
        console_cursor_enable(0);
        console_set_output_hook(0);
        console_reset_region();
        (void)shell_module_call((uint32_t)module_index, "zmodule_redraw");
        graphics_viewport_pop();
    }
}
