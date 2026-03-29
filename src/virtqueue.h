#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <exec/types.h>

/* VirtQueue Descriptor Flags */
#define VIRTQ_DESC_F_NEXT    1  /* Questo descrittore è concatenato al successivo */
#define VIRTQ_DESC_F_WRITE   2  /* Il descrittore è scrivibile (Input per il driver) */
#define VIRTQ_DESC_F_INDIRECT 4 /* Il buffer contiene una tabella di descrittori */

/* VirtQueue Descriptor Table Entry */
struct virtq_desc {
    uint64 addr;    /* Indirizzo fisico del buffer */
    uint32 len;     /* Lunghezza del buffer in byte */
    uint16 flags;   /* Flag di controllo */
    uint16 next;    /* Indice del descrittore successivo (se VRING_DESC_F_NEXT) */
};

/* VirtQueue Available Ring */
struct virtq_avail {
    uint16 flags;
    uint16 idx;
    uint16 ring[];
};

/* VirtQueue Used Ring Entry */
struct virtq_used_elem {
    uint32 id;      /* Indice del descrittore nella tabella */
    uint32 len;     /* Lunghezza totale dei dati scritti */
};

/* VirtQueue Used Ring */
struct virtq_used {
    uint16 flags;
    uint16 idx;
    struct virtq_used_elem ring[];
};

/* VirtQueue Control Structure */
struct virtqueue {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16 queue_index;
    uint16 num;
    uint16 free_head;
    uint16 num_free;
    uint16 last_used_idx;
};

/* Prototipi funzioni in virtqueue.c */
struct virtqueue * virtqueue_alloc(uint16 queue_size, uint16 queue_idx);
void virtqueue_free(struct virtqueue *vq);
void virtqueue_activate(struct PCIDevice *device, struct virtqueue *vq);

/* Gestione Buffer */
int16 virtqueue_add_buffer(struct virtqueue *vq, uint32 addr, uint32 len, uint16 flags);
int16 virtqueue_get_finished(struct virtqueue *vq, uint32 *len);

#endif /* VIRTQUEUE_H */
