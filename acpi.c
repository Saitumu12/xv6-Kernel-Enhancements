#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "acpi.h"

extern volatile uint *lapic;
extern uchar ioapicid;

static uchar
acpi_checksum(uchar *addr, uint len)
{
  uchar sum = 0;
  uint i;

  for(i = 0; i < len; i++)
    sum += addr[i];
  return sum;
}

static struct acpi_rsdp*
find_rsdp(void)
{
  uchar *p, *e;
  uint ebda;

  ebda = (*(ushort*)P2V(0x40E)) << 4;
  if(ebda){
    for(p = P2V(ebda), e = p + 1024; p < e; p += 16){
      if(memcmp(p, "RSD PTR ", 8) == 0 && acpi_checksum(p, 20) == 0)
        return (struct acpi_rsdp*)p;
    }
  }
  for(p = P2V(0xE0000), e = P2V(0x100000); p < e; p += 16){
    if(memcmp(p, "RSD PTR ", 8) == 0 && acpi_checksum(p, 20) == 0)
      return (struct acpi_rsdp*)p;
  }
  return 0;
}

static struct acpi_header*
map_table(uint pa)
{
  struct acpi_header *hdr;

  if(kmap_extend(pa, sizeof(struct acpi_header)) < 0)
    return 0;
  hdr = (struct acpi_header*)P2V(pa);
  if(hdr->length < sizeof(struct acpi_header) || hdr->length > 0x10000)
    return 0;
  if(kmap_extend(pa, hdr->length) < 0)
    return 0;
  if(acpi_checksum((uchar*)hdr, hdr->length) != 0)
    return 0;
  return hdr;
}

int
acpiinit(void)
{
  struct acpi_rsdp *rsdp;
  struct acpi_header *rsdt, *hdr;
  struct acpi_madt *madt;
  struct acpi_madt_lapic *cpu;
  struct acpi_madt_ioapic *io;
  uint *tables;
  uchar *p, *e;
  int i, count, found;

  if((rsdp = find_rsdp()) == 0)
    return -1;

  if((rsdt = map_table(rsdp->rsdtaddr)) == 0)
    return -1;
  if(memcmp(rsdt->signature, "RSDT", 4) != 0)
    return -1;

  count = (rsdt->length - sizeof(struct acpi_header)) / sizeof(uint);
  tables = (uint*)((uchar*)rsdt + sizeof(struct acpi_header));

  madt = 0;
  for(i = 0; i < count; i++){
    if((hdr = map_table(tables[i])) == 0)
      continue;
    if(memcmp(hdr->signature, "APIC", 4) == 0){
      madt = (struct acpi_madt*)hdr;
      break;
    }
  }
  if(madt == 0)
    return -1;

  lapic = (uint*)madt->lapicaddr;

  found = 0;
  p = (uchar*)madt + sizeof(struct acpi_madt);
  e = (uchar*)madt + madt->header.length;
  while(p + 2 <= e){
    if(p[1] < 2 || p + p[1] > e)
      break;
    switch(p[0]){
    case ACPI_MADT_LAPIC:
      cpu = (struct acpi_madt_lapic*)p;
      if((cpu->flags & 1) && ncpu < NCPU){
        cpus[ncpu].apicid = cpu->apicid;
        ncpu++;
        found++;
      }
      break;
    case ACPI_MADT_IOAPIC:
      io = (struct acpi_madt_ioapic*)p;
      ioapicid = io->id;
      break;
    }
    p += p[1];
  }

  if(found == 0)
    return -1;
  return 0;
}
