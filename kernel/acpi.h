#pragma once
#include <stdint.h>
#include <stdbool.h>

/* =========================================================
   Phase 5.3 — ACPI Power Management
   Provides: acpi_init(), acpi_shutdown(), acpi_reboot()
   ========================================================= */

/* ---- RSDP ---- */
typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0,  2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* ACPI 2.0+ only (revision >= 2) */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

/* ---- Generic Address Structure ---- */
typedef struct __attribute__((packed)) {
    uint8_t  address_space; /* 0=Memory, 1=I/O, 2=PCI */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} acpi_gas_t;

/* ---- SDT header (common to all tables) ---- */
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

/* ---- RSDT ---- */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t          tables[];  /* array of 32-bit physical addresses */
} acpi_rsdt_t;

/* ---- XSDT ---- */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint64_t          tables[];  /* array of 64-bit physical addresses */
} acpi_xsdt_t;

/* ---- FADT (partial — only fields we use) ---- */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;   /* <-- PM1a control register (16-bit I/O port) */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved1;
    uint32_t flags;
    acpi_gas_t reset_reg;    /* ACPI 2.0+: reset register */
    uint8_t  reset_value;    /* value to write to reset_reg */
    /* rest omitted — we don't need them */
} acpi_fadt_t;

/* Public API */
bool acpi_init(void);       /* call after identity-map is set up */
void acpi_shutdown(void);   /* write SLP_TYP + SLP_EN → PM1a_CNT; fallback QEMU port */
void acpi_reboot(void);     /* write reset_value → FADT reset_reg; fallback PS/2 */
