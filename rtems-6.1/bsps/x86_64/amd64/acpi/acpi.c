/* SPDX-License-Identifier: BSD-2-Clause */

/**
 * @file
 *
 * @ingroup RTEMSBSPsX8664AMD64
 *
 * @ingroup RTEMSBSPsX8664AMD64EFI
 *
 * @brief ACPI implementation
 */

/*
 * Copyright (C) 2024 Matheus Pecoraro
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <acpi/acpi.h>
#include <rtems/sysinit.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

uint64_t acpi_rsdp_addr = 0;

static bool acpi_tables_initialized = false;

static ACPI_TABLE_HEADER *acpi_get_root_table(
  uint32_t *entry_size,
  uint32_t *entry_count
)
{
  ACPI_TABLE_RSDP *rsdp;
  ACPI_TABLE_HEADER *root;

  if (acpi_rsdp_addr == 0 || entry_size == NULL || entry_count == NULL) {
    return NULL;
  }

  rsdp = (ACPI_TABLE_RSDP *) (uintptr_t) acpi_rsdp_addr;

  if (memcmp(rsdp->Signature, ACPI_SIG_RSDP, sizeof(rsdp->Signature)) != 0) {
    printf("ACPI early: invalid RSDP signature at 0x%" PRIx64 "\n", acpi_rsdp_addr);
    return NULL;
  }

  printf(
    "ACPI early: RSDP rev=%u rsdt=0x%08" PRIx32 " xsdt=0x%016" PRIx64 "\n",
    rsdp->Revision,
    rsdp->RsdtPhysicalAddress,
    rsdp->XsdtPhysicalAddress
  );

  if (rsdp->Revision > 1 && rsdp->XsdtPhysicalAddress != 0) {
    ACPI_TABLE_XSDT *xsdt = (ACPI_TABLE_XSDT *) (uintptr_t) rsdp->XsdtPhysicalAddress;

    root = &xsdt->Header;
    *entry_size = ACPI_XSDT_ENTRY_SIZE;
  } else {
    ACPI_TABLE_RSDT *rsdt = (ACPI_TABLE_RSDT *) (uintptr_t) rsdp->RsdtPhysicalAddress;

    root = &rsdt->Header;
    *entry_size = ACPI_RSDT_ENTRY_SIZE;
  }

  if (root == NULL || root->Length < sizeof(ACPI_TABLE_HEADER)) {
    return NULL;
  }

  *entry_count = (root->Length - sizeof(ACPI_TABLE_HEADER)) / *entry_size;
  printf(
    "ACPI early: root %.4s @ 0x%" PRIxPTR ", length=0x%" PRIx32 ", entries=%" PRIu32 "\n",
    root->Signature,
    (uintptr_t) root,
    root->Length,
    *entry_count
  );

  return root;
}

ACPI_TABLE_HEADER *acpi_get_table(const char *signature)
{
  ACPI_TABLE_HEADER *root;
  uint32_t entry_size;
  uint32_t entry_count;
  uint8_t *entry;

  if (signature == NULL) {
    return NULL;
  }

  root = acpi_get_root_table(&entry_size, &entry_count);
  if (root == NULL) {
    return NULL;
  }

  entry = (uint8_t *) root + sizeof(ACPI_TABLE_HEADER);

  for (uint32_t i = 0; i < entry_count; ++i) {
    uint64_t address = 0;
    ACPI_TABLE_HEADER *table;

    memcpy(&address, entry, entry_size);
    entry += entry_size;

    if (address == 0) {
      continue;
    }

    table = (ACPI_TABLE_HEADER *) (uintptr_t) address;

    if (memcmp(table->Signature, signature, ACPI_NAMESEG_SIZE) == 0) {
      printf(
        "ACPI early: found %.4s @ 0x%" PRIx64 ", length=0x%" PRIx32 "\n",
        signature,
        address,
        table->Length
      );
      return table;
    }
  }

  printf("ACPI early: table %.4s not found\n", signature);
  return NULL;
}

bool acpi_tables_initialize(void)
{
  ACPI_STATUS status;

  printf("ACPI: AcpiInitializeTables begin, rsdp=0x%" PRIx64 "\n", acpi_rsdp_addr);

  status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
  printf("ACPI: AcpiInitializeTables status=0x%x\n", status);

  if (status == (AE_OK)) {
    acpi_tables_initialized = true;
    return true;
  }
  return false;
}

void acpi_walk_subtables(
  ACPI_TABLE_HEADER* table,
  size_t size_of_header,
  void (*handler)(ACPI_SUBTABLE_HEADER*)
)
{
  ACPI_SUBTABLE_HEADER* entry;
  ACPI_SUBTABLE_HEADER* end;

  if (table == NULL) {
    return;
  }

  entry = (ACPI_SUBTABLE_HEADER*) ((uint8_t*) table + size_of_header);
  end = (ACPI_SUBTABLE_HEADER*) ((uint8_t*) table + table->Length);

  while (entry < end) {
    handler(entry);
    entry = (ACPI_SUBTABLE_HEADER*) ((uint8_t*) entry + entry->Length);
  }
}

static void initialize_acpi(void)
{
  ACPI_STATUS status;
  status = AcpiInitializeSubsystem();
  assert(status == (AE_OK));

  if (acpi_tables_initialized == false) {
    status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
    assert(status == (AE_OK));
  }

  status = AcpiLoadTables();
  assert(status == (AE_OK));

  /* System Control Interrupts not supported */
  status = AcpiEnableSubsystem(ACPI_NO_HANDLER_INIT);
  assert(status == (AE_OK));

  /* General Purpose Events not supported */
  status = AcpiInitializeObjects(ACPI_NO_EVENT_INIT);
  assert(status == (AE_OK));
}

RTEMS_SYSINIT_ITEM(
  initialize_acpi,
  RTEMS_SYSINIT_DEVICE_DRIVERS,
  RTEMS_SYSINIT_ORDER_MIDDLE
);
