/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef OP13_BITBANG_H
#define OP13_BITBANG_H
#include <stdint.h>
#include <stddef.h>
struct op13_bitbang_io {
 void *cookie;
 int (*read)(void *,uint32_t,uint32_t *);
 int (*write)(void *,uint32_t,uint32_t);
 int (*delay)(void *,unsigned);
};
struct op13_bitbang {
 struct op13_bitbang_io io;
 uint32_t saved_cfg[4],saved_io[4];
 unsigned owned,ready,poisoned;
};
int op13_bitbang_open(struct op13_bitbang *,const struct op13_bitbang_io *,int);
int op13_bitbang_transfer(struct op13_bitbang *,const uint8_t *,uint8_t *,size_t);
/* Caller holds panel reset low and supplies until this succeeds. */
int op13_bitbang_cleanup(struct op13_bitbang *);
#endif
