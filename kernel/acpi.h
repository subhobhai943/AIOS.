#pragma once
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * ACPI — Phase 5.3
 * Provides acpi_init(), acpi_shutdown(), acpi_reboot()
 * No libc. No external headers beyond stdint/stdbool/stddef.
 * ----------------------------------------------------------------------- */

/* RSDP (Root System Description Pointer) — ACPI 1.0 portion */
typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
} acpi_rsdp_v1_t;

/* RSDP extension for ACPI 2.0+ */
typedef struct __attribute__((packed)) {
    acpi_rsdp_v1_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_v2_t;

/* Generic System Description Table header */
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

/* Generic Address Structure (GAS) */
typedef struct __attribute__((packed)) {
    uint8_t  address_space;  /* 0=system memory, 1=system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} acpi_gas_t;

/* FADT — Fixed ACPI Description Table (fields we care about) */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_cmd_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;   /* PM1a control block (I/O port) */
    uint32_t pm1b_cnt_blk;   /* PM1b control block (may be 0) */
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
    acpi_gas_t reset_reg;    /* ACPI 2.0+: where to write reset value */
    uint8_t  reset_value;    /* value to write to reset_reg */
    uint8_t  reserved2[3];
    /* ACPI 2.0+ extended addresses (64-bit) */
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_cnt_blk;
    acpi_gas_t x_pm1b_cnt_blk;
} acpi_fadt_t;

/* DSDT — needed only to extract _S5 sleep object */
/* We locate it but parse minimally (just scan for _S5_ in AML) */

/* Public API */
bool acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);

/* Optional debug: print detected ACPI info to serial */
void acpi_dump_info(void);
