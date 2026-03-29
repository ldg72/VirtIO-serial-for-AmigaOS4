#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include "virtqueue.h"
#include "virtio_pci.h"

/* Riferimento all'interfaccia Exec globale definita in main.c */
extern struct ExecIFace *IExec;

/* Allinea un indirizzo al prossimo blocco di dimensione Align */
#define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))

/* Alloca e inizializza una VirtQueue */
struct virtqueue * virtqueue_alloc(uint16 queue_size, uint16 queue_idx) {
    struct virtqueue *vq = IExec->AllocVecTags(sizeof(struct virtqueue),
                                              AVT_Type, MEMF_SHARED,
                                              AVT_ClearWithValue, 0,
                                              TAG_DONE);
    if (!vq) return NULL;

    /* Calcoliamo la dimensione totale necessaria per i ring */
    /* 1. Descriptor Table: 16 byte * queue_size */
    /* 2. Available Ring: 6 byte + 2 byte * queue_size */
    /* 3. Used Ring (deve essere allineata a 4096): 6 byte + 8 byte * queue_size */
    
    uint32 desc_size = 16 * queue_size;
    uint32 avail_size = 6 + 2 * queue_size;
    uint32 used_size = 6 + 8 * queue_size;
    
    /* Allociamo spazio contiguo per i ring (richiesto da VirtIO) */
    uint32 total_mem_size = desc_size + avail_size;
    total_mem_size = ALIGN_UP(total_mem_size, VIRTQUEUE_ALIGN);
    total_mem_size += used_size;
    
    void *vring_mem = IExec->AllocVecTags(total_mem_size,
                                         AVT_Type, MEMF_SHARED,
                                         AVT_Alignment, VIRTQUEUE_ALIGN,
                                         AVT_ClearWithValue, 0,
                                         TAG_DONE);
    
    if (!vring_mem) {
        IExec->FreeVec(vq);
        return NULL;
    }

    /* Impostiamo i puntatori interni */
    vq->desc = (struct virtq_desc *)vring_mem;
    vq->avail = (struct virtq_avail *)((uint8 *)vring_mem + desc_size);
    vq->used = (struct virtq_used *)((uint8 *)vring_mem + ALIGN_UP(desc_size + avail_size, VIRTQUEUE_ALIGN));
    
    vq->num = queue_size;
    vq->queue_index = queue_idx;
    vq->num_free = queue_size;
    vq->free_head = 0;
    vq->last_used_idx = 0;

    /* Concateniamo i descrittori inizialmente come lista libera */
    for (int i = 0; i < queue_size - 1; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[queue_size - 1].next = 0;
    vq->desc[queue_size - 1].flags = 0;

    return vq;
}

/* Libera una VirtQueue */
void virtqueue_free(struct virtqueue *vq) {
    if (vq) {
        if (vq->desc) IExec->FreeVec(vq->desc);
        IExec->FreeVec(vq);
    }
}

/* Inizializzazione fisica (PFN) sulla scheda PCI */
void virtqueue_activate(struct PCIDevice *device, struct virtqueue *vq) {
    /* Il PFN (Page Frame Number) per VirtIO Legacy è l'indirizzo fisico / 4096 */
    uint32 physical_addr = (uint32)vq->desc;
    uint32 pfn = physical_addr >> 12;
    
    virtio_pci_setup_queue(device, vq->queue_index, pfn);
}

/* Inserisce un buffer nella tabella dei descrittori */
int16 virtqueue_add_buffer(struct virtqueue *vq, uint32 addr, uint32 len, uint16 flags) {
    if (vq->num_free == 0) return -1;

    uint16 head = vq->free_head;
    struct virtq_desc *desc = &vq->desc[head];

    desc->addr = (uint64)addr;
    desc->len = len;
    desc->flags = flags;
    /* vq->free_head è già il prossimo nella catena */
    vq->free_head = desc->next;
    vq->num_free--;

    /* Aggiorniamo l'Available Ring */
    uint16 avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = head;
    
    /* Memory barrier logica (OS4 non richiede barrier esplicite qui per VirtIO Legacy su Pegasus in QEMU solitamente) */
    vq->avail->idx++;

    return head;
}

/* Recupera un buffer elaborato dall'host dall'Used Ring */
int16 virtqueue_get_finished(struct virtqueue *vq, uint32 *len) {
    if (vq->last_used_idx == vq->used->idx) {
        return -1; /* Nulla di nuovo */
    }

    uint16 used_idx = vq->last_used_idx % vq->num;
    struct virtq_used_elem *used_elem = &vq->used->ring[used_idx];

    uint16 descriptor_idx = used_elem->id;
    if (len) *len = used_elem->len;

    /* Rimettiamo il descrittore nella lista libera */
    vq->desc[descriptor_idx].next = vq->free_head;
    vq->free_head = descriptor_idx;
    vq->num_free++;

    vq->last_used_idx++;
    return descriptor_idx;
}
