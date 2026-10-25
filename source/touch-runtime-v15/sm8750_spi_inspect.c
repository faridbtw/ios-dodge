/* SPDX-License-Identifier: BSD-2-Clause */
#include "sm8750_spi_inspect.h"
struct spi_prefix { uint64_t revision; void *open,*transfer,*close; };
static const efi_guid spi_guid={0x4c7ffd28,0x6a06,0x4425,{0x9e,0xe2,0x67,0x6e,0xbc,0x08,0x96,0x83}};
static int equal(const uint8_t *a,const uint8_t *b,size_t n)
{ for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
int sm8750_loader_config_inspect(efi_boot_services *bs,int enabled,
 struct sm8750_config_protocol_report *r)
{
 static const efi_guid cfg={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
 void *p=0;
 if(!bs||!r) return -5;
 if(!enabled) return -3;
 *r=(struct sm8750_config_protocol_report){0};
 r->locate_status=bs->locate_protocol(&cfg,0,&p);
 if(EFI_ERROR(r->locate_status)||!p) return -3;
 r->interface_address=(uintptr_t)p;
 /* Protocol is a published 0x148-byte copy, from exact EnvDxe function0x2c4c.
  * Do not assume slot0 is revision: record without interpreting any slot. */
 const volatile uint8_t *b=p;
 for(unsigned i=0;i<41;i++) {
  uint64_t v=0;
  for(unsigned j=0;j<8;j++) v|=(uint64_t)b[8*i+j]<<(8*j);
  r->slots[i]=v;
 }
 return 0;
}
int sm8750_loader_spi_inspect(efi_boot_services *bs,int enabled,
 const void *image_base,size_t mapped_size,struct sm8750_spi_topology *r)
{
 struct spi_prefix *p=0; const uint8_t *b=image_base;
 static const uint8_t open_code[32]={0xfd,0x7b,0xbe,0xa9,0xf3,0x0b,0x00,0xf9,
  0xfd,0x03,0x00,0x91,0xf3,0x03,0x01,0xaa,0xa1,0x73,0x00,0x91,
  0xa2,0x63,0x00,0x91,0x3b,0x0a,0x00,0x94,0x1f,0x1c,0x00,0x72};
 static const uint8_t mapper_code[32]={0xfd,0x7b,0xbd,0xa9,0xf6,0x57,0x01,0xa9,
  0xf4,0x4f,0x02,0xa9,0xfd,0x03,0x00,0x91,0x56,0x00,0x00,0xb0,
  0xf3,0x03,0x02,0xaa,0xf4,0x03,0x01,0xaa,0xf5,0x03,0x00,0x2a};
 if(!bs||!r) return -5;
 if(!enabled) return -3;
 *r=(struct sm8750_spi_topology){0};
 if(!b||mapped_size<0xf000||mapped_size>0x100000) return -5;
 if(EFI_ERROR(bs->locate_protocol(&spi_guid,0,(void **)&p))||!p) return -3;
 r->revision=p->revision; r->image_size=(uint32_t)mapped_size;
 if(p->revision!=0x10000||(uintptr_t)p->open!=(uintptr_t)b+0x26d0||
    (uintptr_t)p->transfer!=(uintptr_t)b+0x2784||(uintptr_t)p->close!=(uintptr_t)b+0x2714||
    !equal(b+0x26d0,open_code,32)||!equal(b+0x4fd4,mapper_code,32)||
    b[0x9c09]!=3||b[0x9c0a]!=1||b[0x9dc0]!=0||b[0x9dc1]!=3) return -3;
 /* Volatile reads because a firmware owner may initialize the cache. Do not
  * initialize it ourselves: the DAL helper is not a public capability API. */
 const volatile uint8_t *v=b;
 r->initialized=v[0xd300];
 for(unsigned i=0;i<4;i++) r->wrapper_counts[i]=v[0xd2fc+i];
 if(r->initialized!=v[0xd300]||!(r->initialized&1)) return -6;
 for(unsigned i=0;i<4;i++) if(r->wrapper_counts[i]>16) return -3;
 if(r->wrapper_counts[1]<=4) return -3;
 r->logical_instance=1u+r->wrapper_counts[0]+4u;
 /* This maps only the logical tuple TOP_QUP1 / SE4. No DT-to-DAL physical
  * equivalence is implied, so physical_address_verified intentionally stays0. */
 return 0;
}
