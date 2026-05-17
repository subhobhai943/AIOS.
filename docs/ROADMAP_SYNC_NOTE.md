# Updated roadmap synced with implemented LLM and GUI features

- Phase 7.2–7.9: core LLM engine (`kernel/llm/ops.c`, `attention.c`, `transformer.c`,
  `model.c`, `loader.c`, `tokenizer.c`, `quant.c`, `inference.c`) implemented.
- Phase 10.3: GUI input wiring from `keyboard.c` / `mouse.c` into `kernel/gui/input.c`
  completed via `keyboard_set_gui_callback` / `mouse_set_gui_callback` and
  `gui_wiring_activate()`.
- Phase 10.6: `startx` / `gui` shell commands implemented in
  `kernel/shell/shell.c`, calling `gui_input_enable()` and `gui_wm_start()`.

Roadmap checkboxes and summary sections have been updated accordingly.
