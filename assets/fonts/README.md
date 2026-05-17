# AIOS Bitmap Font Assets

This directory will contain bitmap font files used by the GUI layer for
future theming and richer text rendering (full ASCII, multiple sizes).

The kernel currently uses a built-in 8×16 debug font defined in
`kernel/gfx/font.c`. This directory is reserved for on-disk fonts that can
be loaded later (e.g., from the initrd or a filesystem) without recompiling
the kernel.

Font file format and concrete assets will be added in a later Phase 10.2
task.
