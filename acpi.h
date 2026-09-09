#define ACPI_MADT_LAPIC   0
#define ACPI_MADT_IOAPIC  1

struct acpi_rsdp {
  char signature[8];
  uchar checksum;
  char oemid[6];
  uchar revision;
  uint rsdtaddr;
} __attribute__((packed));

struct acpi_header {
  char signature[4];
  uint length;
  uchar revision;
  uchar checksum;
  char oemid[6];
  char oemtableid[8];
  uint oemrevision;
  uint creatorid;
  uint creatorrevision;
} __attribute__((packed));

struct acpi_madt {
  struct acpi_header header;
  uint lapicaddr;
  uint flags;
} __attribute__((packed));

struct acpi_madt_lapic {
  uchar type;
  uchar length;
  uchar procid;
  uchar apicid;
  uint flags;
} __attribute__((packed));

struct acpi_madt_ioapic {
  uchar type;
  uchar length;
  uchar id;
  uchar reserved;
  uint addr;
  uint gsibase;
} __attribute__((packed));
