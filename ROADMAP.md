# AIOS — AI Operating System Roadmap
@@
 ### 11.3 — GUI Terminal
-- ⬜ `kernel/apps/terminal_gui.c` — shell in a GUI window
+- ✅ `kernel/apps/terminal_gui.c`, `kernel/apps/terminal_gui.h` — shell in a GUI window
@@
 ### 11.4 — Settings
 - ⬜ `kernel/apps/settings.c` — tabbed config panel
@@
-| **File Explorer** | `apps/explorer.c` | ✅ **11.2** |
-| Quantization | `llm/quant.c` | ⬜ 7.8 |
-| GUI Terminal | `apps/terminal_gui.c` | ⬜ **NEXT → 11.3** |
+| **File Explorer** | `apps/explorer.c` | ✅ **11.2** |
+| GUI Terminal | `apps/terminal_gui.c` | ✅ **11.3** |
+| Quantization | `llm/quant.c` | ⬜ 7.8 |
@@
-1. **Phase 11.3 — GUI Terminal** ← **NEXT (GUI track)**  \
-   `kernel/apps/terminal_gui.c` — terminal emulator in a window, backing onto existing shell.
-
-2. **Phase 7.8 — Quantization** (LLM track)  \
+1. **Phase 7.8 — Quantization** (LLM track)  \
    `kernel/llm/quant.c` — on-the-fly Q8_0/Q4_K dequant matmul.
@@
 kernel/llm/
@@
   quant.c/h       ← ⬜ 7.8
   inference.c/h   ← ⬜ 7.9
 
 kernel/apps/
   notepad.c/h     ← ✅ 11.1
   explorer.c/h    ← ✅ 11.2
-  terminal_gui.c  ← ⬜ NEXT 11.3
+  terminal_gui.c/h← ✅ 11.3
   settings.c      ← ⬜ 11.4
   ai_chat.c       ← ⬜ 11.5
@@
-*Last updated: May 2026 — Phase 11.2 complete (File Explorer: path header, list view, keyboard + mouse navigation, Notepad integration). Next: Phase 11.3 — GUI Terminal (`kernel/apps/terminal_gui.c`).*
+*Last updated: May 2026 — Phase 11.3 complete (GUI Terminal window mirroring shell output + input). Next: Phase 7.8 — Quantization (`kernel/llm/quant.c`).*
