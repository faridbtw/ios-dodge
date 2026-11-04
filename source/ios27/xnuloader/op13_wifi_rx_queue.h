/* SPDX-License-Identifier: BSD-2-Clause
 * Private fixed 64-slot CE buffer FIFO, independent of hardware access.
 * Commit a completion only after parsing its buffer; commit posting only
 * after DMA publication succeeds. Any I/O failure ends the hardware session.
 */
#ifndef OP13_WIFI_RX_QUEUE_H
#define OP13_WIFI_RX_QUEUE_H
struct op13_rx_queue {
 unsigned slot[64],pending[64],read_slot,write_slot,posted,head,count;
};
static void op13_rx_queue_init(struct op13_rx_queue *q)
{
 for(unsigned i=0;i<64;i++)q->slot[i]=i;
 q->read_slot=0;q->write_slot=63;q->posted=63;q->head=0;q->count=0;
}
static int op13_rx_queue_buffer(const struct op13_rx_queue *q,unsigned *id)
{
 if(!q||!id||!q->posted||q->posted>63||q->read_slot>=64||q->slot[q->read_slot]>=64)return -1;
 *id=q->slot[q->read_slot];return 0;
}
static int op13_rx_queue_complete(struct op13_rx_queue *q)
{
 unsigned id;if(op13_rx_queue_buffer(q,&id)||q->count>=63||q->head>=64)return -1;
 q->pending[(q->head+q->count)&63]=id;q->count++;
 q->posted--;q->read_slot=(q->read_slot+1)&63;return 0;
}
static int op13_rx_queue_refill(const struct op13_rx_queue *q,unsigned hardware_tp,unsigned *slot,unsigned *id)
{
 if(!q||!slot||!id||q->posted>63||q->count>63||q->write_slot>=64||q->head>=64||hardware_tp>=128||(hardware_tp&1))return -1;
 if(!q->count||q->posted==63||(((q->write_slot+1)&63)*2)==hardware_tp)return 0;
 *slot=q->write_slot;*id=q->pending[q->head];if(*id>=64)return -1;return 1;
}
static int op13_rx_queue_post(struct op13_rx_queue *q)
{
 if(!q||!q->count||q->count>63||q->posted>=63||q->head>=64||q->write_slot>=64)return -1;
 unsigned id=q->pending[q->head];if(id>=64)return -1;
 q->slot[q->write_slot]=id;q->write_slot=(q->write_slot+1)&63;q->posted++;
 q->head=(q->head+1)&63;q->count--;return 0;
}
#endif
