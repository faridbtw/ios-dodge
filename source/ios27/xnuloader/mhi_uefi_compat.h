/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_MHI_UEFI_COMPAT_H
#define OP13_MHI_UEFI_COMPAT_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#define EIO 5
#define ENXIO 6
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#define EMSGSIZE 90
#define ENOTCONN 107
#define ETIMEDOUT 110
void *memset(void *,int,size_t);
void *memcpy(void *,const void *,size_t);
int op13_mhi_vsnprintf(char *,size_t,const char *,va_list);
#define vsnprintf op13_mhi_vsnprintf
#endif
