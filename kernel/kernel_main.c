/* ============================================================
 * AIOS — Kernel Main
 * Phase 10.2: GUI text rendering groundwork
 * ============================================================ */

#include "include/vga.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pmm.h"
#include "include/vmm.h"
#include "include/heap.h"
#include "include/keyboard.h"
#include "include/mouse.h"
#include "include/pit.h"
#include "include/panic.h"
#include "apic.h"
#include "serial.h"
#include "pci.h"
#include "ahci.h"
#include "fat32.h"
#include "fs/vfs.h"
#include "task.h"
#include "sched.h"
#include "acpi.h"
#include "gfx/framebuffer.h"  /* Phase 10.1 */
#include "gfx/colors.h"       /* Phase 10.x UI colours */
#include "gfx/font.h"         /* Phase 10.2 text rendering */

#include <stdint.h>

#define MULTIBOOT2_MAGIC      0x36D76289u
#define MB2_TAG_TYPE_MMAP     6u
#define MB2_TAG_TYPE_END      0u

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

#define HEAP_SIZE   (2u * 1024u * 1024u)

extern void pf_handler(interrupt_frame_t *frame);

/* ── Helpers ─────────────────────────────────────────────── */
static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    klog("[ OK ] "); klog(msg); klog("\r\n");
}
static void print_warn(const char *msg)
{
    vga_puts_color("[WARN] ", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    klog("[WARN] "); klog(msg); klog("\r\n");
}
static void print_fail(const char *msg)
{
    vga_puts_color("[FAIL] ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    kernel_panic(msg);
}

/* ── MB2 mmap scanner ─────────────────────────────────────── */
static uint64_t find_mmap_tag(uint32_t mb2_addr, uint32_t *tag_size_out)
{
    uint32_t total = *(uint32_t *)(uintptr_t)mb2_addr;
    uint32_t off   = 8;
    while (off < total) {
        uint32_t type = *(uint32_t *)(uintptr_t)(mb2_addr + off);
        uint32_t size = *(uint32_t *)(uintptr_t)(mb2_addr + off + 4);
        if (type == MB2_TAG_TYPE_END) break;
        if (type == MB2_TAG_TYPE_MMAP) {
            if (tag_size_out) *tag_size_out = size;
            return (uint64_t)(mb2_addr + off);
        }
        off += (size + 7u) & ~7u;
    }
    return 0;
}

/* ── #DE regression test handler ─────────────────────────── */
static void de_test_handler(interrupt_frame_t *frame)
{
    vga_puts_color("  [ OK ] #DE handler fired — vector=0x",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puthex(frame->int_num);
    vga_puts_color("  RIP=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->rip); vga_putchar('\n');
    frame->rip += 3;
}

/* ── PIT IRQ handler — now calls sched_tick() ───────────── */
static void pit_irq_handler(interrupt_frame_t *f)
{
    (void)f;
    pit_tick();
    apic_send_eoi();
    sched_tick();
}
static void kbd_isr(interrupt_frame_t *f)
{
    (void)f; keyboard_handle_irq(); apic_send_eoi();
}
static void mouse_isr(interrupt_frame_t *f)
{
    (void)f; mouse_handle_irq(); apic_send_eoi();
}

static volatile int g_task_a_done = 0;
static volatile int g_task_b_done = 0;
static volatile int g_task_c_done = 0;

static void task_a(void)
{
    for (int i = 0; i < 5; i++) {
        klog("[TASK A] tick i=");
        klog_dec((uint32_t)i);
        klog("\r\n");
        vga_puts_color("A", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sched_sleep(250);
    }
    vga_puts_color("[A done]", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    g_task_a_done = 1;
    sched_exit();
}

static void task_b(void)
{
    for (int i = 0; i < 5; i++) {
        klog("[TASK B] tick i=");
        klog_dec((uint32_t)i);
        klog("\r\n");
        vga_puts_color("B", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        sched_sleep(250);
    }
    vga_puts_color("[B done]", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    g_task_b_done = 1;
    sched_exit();
}

static void task_c(void)
{
    for (int i = 0; i < 5; i++) {
        klog("[TASK C] tick i=");
        klog_dec((uint32_t)i);
        klog("\r\n");
        vga_puts_color("C", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        sched_sleep(250);
    }
    vga_puts_color("[C done]", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    g_task_c_done = 1;
    sched_exit();
}

void kernel_main(uint32_t magic, uint32_t addr)
{
    vga_init();
    serial_init(SERIAL_COM1, 115200);

    vga_puts_color(
        "====================================================\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts_color(
        "  AIOS  Autonomous Intelligent Operating System\n",
        VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts_color(
        "  Phase 10.2: GUI text rendering groundwork\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    klog("\r\n=== AIOS Phase 10.2 boot ===\r\n");

    if (magic == MULTIBOOT2_MAGIC)
        print_ok("Multiboot2 magic OK");
    else
        print_warn("MB2 magic mismatch — continuing");

    gdt_init();
    print_ok("GDT: null/kcode/kdata/ucode/udata + TSS");

    idt_init();
    print_ok("IDT: 256 gates, exception dump active");

    idt_register_handler(14, pf_handler);
    print_ok("#PF handler installed");

    idt_register_handler(0, de_test_handler);
    vga_puts_color("  [TEST] Triggering #DE...\n",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    __asm__ volatile ("xor %%rcx,%%rcx; div %%rcx" ::: "rax","rdx","rcx");
    print_ok("#DE test PASSED");
    idt_register_handler(0, 0);

    vmm_init();
    print_ok("VMM: 4-level paging, 64 MB identity map");

    if (magic == MULTIBOOT2_MAGIC) {
        uint32_t mmap_size = 0;
        uint64_t mmap_tag  = find_mmap_tag(addr, &mmap_size);
        if (!mmap_tag) print_fail("MB2 mmap tag not found");
        uint64_t kstart = (uint64_t)(uintptr_t)&_kernel_start;
        uint64_t kend   = (uint64_t)(uintptr_t)&_kernel_end;
        pmm_init(mmap_tag, mmap_size, kstart, kend);
        print_ok("PMM: bitmap allocator ready");
    } else {
        print_warn("PMM skipped — heap uses static region");
    }

    {
        uint64_t kend_aligned = ((uint64_t)(uintptr_t)&_kernel_end
                                 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        heap_init(kend_aligned, HEAP_SIZE);
        print_ok("Heap: 2 MB kernel heap ready");
    }

    apic_init();
    print_ok("APIC: legacy PIC dead, LAPIC active");

    idt_register_handler(0x20, pit_irq_handler);
    ioapic_route(0, 0x20, 0);
    pit_init(1000);
    print_ok("PIT: 1000 Hz, IRQ0 → vec 0x20");

    idt_register_handler(0x21, kbd_isr);
    ioapic_route(1, 0x21, 0);
    keyboard_init();
    print_ok("Keyboard: IRQ1 → vec 0x21");

    idt_register_handler(0x2C, mouse_isr);
    ioapic_route(12, 0x2C, 0);
    mouse_init();
    print_ok("Mouse: IRQ12 → vec 0x2C");

    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI)");

    /* Phase 10.1/10.2: framebuffer + text smoke-test. */
    if (magic == MULTIBOOT2_MAGIC && fb_init_from_multiboot(addr)) {
        framebuffer_t *fb = fb_get();
        const gui_font_t *font = font_load_builtin();

        /* Clear desktop to a dark background and draw a top taskbar-like
         * rectangle with an "AIOS" label centered in it. */
        fb_clear(UI_COLOR_DESKTOP_BG);
        fb_fill_rect(0, 0, fb->width, 40, UI_COLOR_TASKBAR_BG);
        fb_draw_rect(0, 0, fb->width, 40, UI_COLOR_ACCENT);

        /* Vertically center text within the 40px bar. */
        uint32_t text_y = 0;
        if (font->height < 40u) {
            text_y = (40u - font->height) / 2u;
        }
        font_draw_string_centered(fb, font, 0, text_y, fb->width,
                                  "AIOS Desktop", UI_COLOR_TEXT_FG,
                                  UI_COLOR_TASKBAR_BG);

        print_ok("Framebuffer+font: GUI banner with text rendered (Phase 10.2)");
    } else {
        print_warn("Framebuffer tag missing or unsupported — staying in VGA text mode");
    }

    vga_puts_color("\n  [TEST] pit_sleep_ms(200)...\n",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    {
        uint64_t t0 = pit_get_ticks();
        pit_sleep_ms(200);
        uint64_t elapsed = pit_get_ticks() - t0;
        if (elapsed >= 180ULL && elapsed <= 250ULL)
            print_ok("PIT tick test PASSED");
        else
            print_warn("Tick count out of range");
    }

    vga_putchar('\n');
    vga_puts_color("--- Phase 3.1: PCI ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    pci_init();
    pci_dump();
    print_ok("PCI enumeration complete");

    vga_puts_color("--- Phase 5.3: ACPI ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (acpi_init()) {
        acpi_dump_info();
        print_ok("ACPI: RSDP/FADT parsed, power management ready");
    } else {
        print_warn("ACPI init failed — reboot/shutdown use fallbacks");
    }

    vga_puts_color("--- Phase 3.2: AHCI ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    int ahci_ok   = 0;
    int ahci_port = -1;
    if (ahci_init() == 0) {
        for (int p = 0; p < 32; p++) {
            if (ahci_port_available(p)) {
                ahci_sector0_test(p);
                ahci_port = p;
                break;
            }
        }
        print_ok("AHCI driver initialised");
        ahci_ok = 1;
    } else {
        print_warn("AHCI init failed or no controller present");
    }

    vga_puts_color("--- Phase 3.3: FAT32 + VFS ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (ahci_ok && ahci_port >= 0) {
        if (fat32_init(ahci_port, 0) == 0) {
            fat32_sector0_test();
            vfs_init(2);
            {
                int fd = vfs_open("/TEST.TXT");
                if (fd >= 0) {
                    static uint8_t vbuf[64];
                    int got = vfs_read(fd, vbuf, 64);
                    if (got > 0) print_ok("VFS smoke-test: read OK");
                    else         print_warn("VFS: read 0 bytes");
                    vfs_close(fd);
                } else {
                    print_warn("VFS: TEST.TXT not found");
                }
            }
            print_ok("FAT32 + VFS layer ready");
        } else {
            print_warn("FAT32 init failed");
        }
    } else {
        print_warn("FAT32/VFS skipped — no AHCI disk");
    }

    vga_putchar('\n');
    vga_puts_color("--- Phase 4.1+4.2: Scheduler ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    task_init();
    print_ok("Task table initialised (boot task = PID 0)");

    sched_init();
    print_ok("Scheduler initialised (idle task created)");

    {
        task_t *ta = task_create(task_a, 8192, "task_a");
        task_t *tb = task_create(task_b, 8192, "task_b");
        task_t *tc = task_create(task_c, 8192, "task_c");
        KERNEL_ASSERT(ta && tb && tc, "test task creation failed");
        sched_add(ta);
        sched_add(tb);
        sched_add(tc);
    }
    print_ok("Tasks A/B/C created and enqueued");

    vga_puts_color(
        "  Scheduler running. Output (A/B/C) shows preemption:\n",
        VGA_COLOR_BROWN, VGA_COLOR_BLACK);

    while (!g_task_a_done || !g_task_b_done || !g_task_c_done) {
        sched_yield();
    }
    vga_putchar('\n');
    print_ok("Scheduler test PASSED — tasks A/B/C completed");

    vga_putchar('\n');
    vga_puts_color(
        "AIOS Phase 10.2 boot complete. GUI text groundwork ready.\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    klog("Phase 10.2 boot complete. GUI text groundwork ready.\r\n");

    for (;;) __asm__ volatile ("hlt");
}
