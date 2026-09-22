# Proposal: Combo editing in ZMK Studio

Status: **design + skeleton**, not built. Tracks zmkfirmware/zmk-studio#93 and the 💡 "Configuring
combos" row in `docs/docs/features/studio.md`.

This directory holds everything needed to start the implementation across the three repos:

| Path                          | Target repo              | Purpose                                                       |
| ----------------------------- | ------------------------ | ------------------------------------------------------------- |
| `proto/combos.proto`          | zmk-studio-messages      | New `zmk.combos` package (requests, responses, notifications) |
| `proto/combos.options.in`     | zmk-studio-messages      | nanopb bound for `key_positions` (from Kconfig)               |
| `proto/studio.proto.patch`    | zmk-studio-messages      | Adds `combos = 6` to the three top-level `oneof`s             |
| `firmware/combos.h`           | zmk → `app/include/zmk/` | Runtime combo API contract that `combo.c` must implement      |
| `firmware/combos_subsystem.c` | zmk → `app/src/studio/`  | RPC handlers, written against the proto above                 |

## Prerequisites already landed on this branch

- **`fix(combos): honor the physical layout position map`** — combos are now translated through
  `selected_to_stock_map` when the lookup is built, and rebuilt on
  `zmk_physical_layout_selection_changed`. Without this, drawing combos on the Studio layout picture
  would show them on the wrong keys the moment a user switches layouts.
- `zmk,behavior-select-physical-layout` test behavior + two `native_sim` tests under
  `app/tests/combo/position-map*` that any combo-editing work can extend.

## Design decisions

### Reserved slots, not dynamic allocation

`combos[]` is a compile-time array and the matcher's bitmask tables (`candidates`, `combo_lookup`)
are sized from it. Rather than fight that, reuse the idiom that already exists for layers:

```dts
combos {
    compatible = "zmk,combos";

    combo_esc { key-positions = <0 1>; bindings = <&kp ESC>; };      // stock, editable
    studio_a  { status = "reserved"; key-positions = <0 0 0 0>; bindings = <&none>; };
    studio_b  { status = "reserved"; key-positions = <0 0 0 0>; bindings = <&none>; };
};
```

- Reserved slots are the user's way of saying "let Studio create up to N combos of up to M keys".
  `key-positions` length on a reserved node feeds `MAX_COMBO_KEYS` exactly as an okay node does.
- Stock combos can be **edited** (retune the timeout, swap the binding) but not **cleared**, so a
  keyboard never loses a combo its designer considered essential (e.g. bootloader). This matches
  how stock layers can be renamed/rebound but the default layer can't be removed.

### Sort order is an implementation detail, kept internal

`combo.c` relies on shortest-first ordering for candidate filtering. After runtime edits the array
can't be re-sorted in place without invalidating the `combo_idx` that `active_combos[]` and
`ZMK_VIRTUAL_KEY_POSITION_COMBO()` hold. So the slot index stays stable and a separate sorted index
array is rebuilt on each edit; every loop in `combo.c` that walks `combos[]` walks the index instead.
This is invisible to the RPC layer — `ComboSlot.index` is the stable slot number.

### One save button

No `combos.save_changes`. The existing `keymap.save_changes` / `discard_changes` /
`check_unsaved_changes` handlers grow three lines each to also call the `zmk_combos_*` equivalents.
Both subsystems raise `unsaved_changes_status_changed`, so the UI keeps a single dirty indicator.

### Positions are stock-layout positions on the wire

This is the same convention as devicetree, but **differs from the keymap RPC**, which uses
_selected_-layout positions and maps them in `zmk_keymap_set_layer_binding_at_idx()`. Stock positions
were chosen for combos because (a) a combo may reference a key not present in the selected layout
and there is no selected-layout number for it, and (b) `get_physical_layouts` already gives the UI
everything it needs to translate. **Revisit before freezing the proto** — the alternative is to add
`repeated uint32 selected_key_positions` alongside and let the firmware translate, at the cost of (a).

### Buffer sizes

## Firmware work remaining (in `combo.c`)

1. `#if IS_ENABLED(CONFIG_ZMK_COMBOS_SETTINGS_STORAGE)`: make `combos[]` non-const, add
   `static const struct combo_cfg stock_combos[]`, a `populated` bitmap, a `pending` bitmap and the
   sorted `order[]` index. Include reserved children via `DT_INST_FOREACH_CHILD` (not
   `_STATUS_OKAY`) under this flag, the same trick `ZMK_KEYMAP_LAYERS_FOREACH` uses.
2. Implement every function in `combos.h`. Validation order: index → key count → range → duplicates
   → behavior lookup → `zmk_behavior_validate_binding()`.
3. `SETTINGS_STATIC_HANDLER_DEFINE(combos, "combos", …)` loader, plus save/discard/reset mirroring
   `keymap.c:463-690`.
4. Wire `combos_subsystem.c` into `app/src/studio/CMakeLists.txt` under
   `CONFIG_ZMK_COMBOS_SETTINGS_STORAGE`; extend the three keymap save/discard/check handlers.
5. Tests under `app/tests/studio/combos/`: add a `zmk,behavior-set-combo` test behavior (same shape
   as `behavior_set_layer_binding_at_idx.c`) and cover: fill reserved slot → fires; edit stock
   timeout → respects new timeout; clear reserved → stops firing; clear stock → `-EPERM`; save →
   survives `settings_load_subtree("combos")`.

## UI design (zmk-studio fork)

The devicetree mental model ("a list of integers") is the source of most combo pain. Studio has the
keyboard picture, so:

- **Combos tab** beside the layer list. Selecting a combo highlights its keys on the layout and
  draws a connector between them. "New combo" → click 2+ keys → pick a behavior in the existing
  `BehaviorBindingPicker`. Integers never appear.
- **In-context badges** on every key that participates in a combo; hover shows the output. Today
  users forget which keys carry combos — this is the single largest UX win.
- **Live conflict detection** (pure UI): identical position sets, a combo whose keys are not all in
  the current physical layout (surfaces #3228 to the user), a `layer_mask` that excludes every
  layer, `require_prior_idle_ms` set on a combo with no layer restriction (common footgun).
- **Layer scoping as checkboxes** against real layer names; the UI converts to/from `layer_mask`.
- **Advanced disclosure** (collapsed): `timeout_ms`, `require_prior_idle_ms`, `slow_release`, with
  the same one-line explanations as the ZMK docs.
- **Stock combos** render with a lock glyph and a disabled "Delete", tooltip "Defined in firmware;
  can be edited but not removed".
- Every mutation goes through the existing `useUndoRedo` `DoCallback` so Ctrl-Z works across
  keymap and combo edits alike.

## Prior art to review before implementing

danielsvane/zmk + danielsvane/zmk-studio, branch `customkeyboards` (MIT / Apache, posted on
zmk-studio#93 on 2026-09-01) implements combo _and_ behavior editing. Read it for approach and
pitfalls; it is licence-compatible, unlike the QMK/Vial repos in the parent directory which must
**not** be consulted per ZMK's clean-room policy.

`SetComboRequest` with 4 positions + binding + timing is ~35–45 bytes encoded, exceeding
`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=30`. The Kconfig default must go to at least 128 in the same
commit that adds the subsystem. TX is streamed via `pb_callback` so `GetCombosResponse` size doesn't
matter.
