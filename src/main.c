#include <exec/types.h>
#include <exec/exec.h>
#include <exec/resident.h>
#include <exec/interfaces.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <exec/exectags.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <interfaces/dos.h>

#include "virtio_pci.h"
#include "virtqueue.h"

/* Dichiarazione globale dell'interfaccia Exec (necessaria per OS4) */
extern struct ExecIFace *IExec;
struct MMUIFace *IMMU = NULL;
struct DOSIFace *IDOS = NULL;
struct Library *DosBase = NULL;

static uint32 ascii_tolower(uint32 c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }

    return c;
}

static BOOL streq_nocase(CONST_STRPTR a, CONST_STRPTR b) {
    if (!a || !b) {
        return FALSE;
    }

    while (*a && *b) {
        if (ascii_tolower((uint8)*a) != ascii_tolower((uint8)*b)) {
            return FALSE;
        }

        a++;
        b++;
    }

    return (*a == '\0' && *b == '\0');
}

/* Estensione del device base per memorizzare lo stato hardware */
struct VirtioBase {
    struct Library      lib_Base;
    struct PCIDevice    *pci_Dev;
    struct virtqueue    *rx_queue;
    struct virtqueue    *tx_queue;
    uint32              features;
    struct Interrupt    isr_Node;
    uint32              irq_Num;
    struct Task         *dev_Task;
    struct Task         *task_owner;
    struct IORequest    *rx_reqs[64]; /* Mapping Descriptor -> IORequest */
    struct IORequest    *tx_reqs[64];
    uint32              sig_bit;
    uint32              task_done_sig_bit;
    APTR                seg_List;
    struct LibraryManagerInterface lib_manager_iface;
    struct DeviceManagerInterface  device_manager_iface;
};

/* Entry point iniziale del binario: deve essere la prima cosa nel file */
int32 _start(void) {
    return -1;
}

/* Versione e Info del Device */
#define DEVICE_NAME "virtio-serial.device"
#define DEVICE_VERSION 1
#define DEVICE_REVISION 0
#define DEVICE_ID_STRING "virtio-serial.device 1.0 (29.03.2026)"
#define VIRTIO_MAX_QUEUE_DESCRIPTORS 64

/* Prototipi obbligatori per OS4 */
struct Library * APICALL LibInit(struct Library *library, APTR seglist, struct Interface *exec);
uint32 APICALL LibObtain(struct LibraryManagerInterface *Self);
uint32 APICALL LibRelease(struct LibraryManagerInterface *Self);
struct Library * APICALL LibOpen(struct LibraryManagerInterface *Self, uint32 version);
APTR APICALL LibClose(struct LibraryManagerInterface *Self);
APTR APICALL LibExpunge(struct LibraryManagerInterface *Self);
struct Interface * APICALL LibGetInterface(struct LibraryManagerInterface *Self, STRPTR name, uint32 version, struct TagItem *taglist);

/* Entry point del Device */
VOID APICALL DevOpen(struct DeviceManagerInterface *Self, struct IORequest *ior, uint32 unit, uint32 flags);
APTR APICALL DevClose(struct DeviceManagerInterface *Self, struct IORequest *ior);
VOID APICALL DevBeginIO(struct DeviceManagerInterface *Self, struct IORequest *ior);
VOID APICALL DevAbortIO(struct DeviceManagerInterface *Self, struct IORequest *ior);

/* Tabelle delle funzioni (VTable) */
STATIC CONST APTR lib_manager_vectors[] = {
    (APTR)LibObtain,
    (APTR)LibRelease,
    NULL,
    NULL,
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibGetInterface,
    (APTR)-1
};

STATIC CONST APTR device_manager_vectors[] __attribute__((used)) = {
    (APTR)LibObtain,
    (APTR)LibRelease,
    NULL,
    NULL,
    (APTR)DevOpen,
    (APTR)DevClose,
    (APTR)LibExpunge,
    (APTR)LibGetInterface,
    (APTR)DevBeginIO,
    (APTR)DevAbortIO,
    (APTR)-1
};

/* Tabelle di inizializzazione per RTF_AUTOINIT */
STATIC CONST struct TagItem lib_manager_tags[] = {
    { MIT_VectorTable, (uint32)lib_manager_vectors },
    { TAG_DONE, 0 }
};

/* Dati per l'auto-inizializzazione della InitTable */
STATIC CONST uint32 lib_init_table[] = {
    sizeof(struct VirtioBase),
    (uint32)lib_manager_tags,
    0, /* Nessun data table aggiuntivo */
    (uint32)LibInit
};

/* ROMTag (Resident Structure) */
STATIC CONST struct Resident romtag = {
    RTC_MATCHWORD,
    (struct Resident *)&romtag,
    (struct Resident *)&romtag + 1,
    RTF_NATIVE | RTF_AUTOINIT,
    DEVICE_VERSION,
    NT_DEVICE,
    0,
    (STRPTR)DEVICE_NAME,
    (STRPTR)DEVICE_ID_STRING,
    (APTR)lib_init_table
};

void virtio_pci_notify(struct PCIDevice *device, uint16 queue_idx);
int16 virtqueue_add_buffer(struct virtqueue *vq, uint32 addr, uint32 len, uint16 flags);
int16 virtqueue_get_finished(struct virtqueue *vq, uint32 *len);
void DeviceTaskFunc(uint32 base_addr);
struct PCIDevice * find_virtio_serial_device();
struct virtqueue * virtqueue_alloc(uint16 queue_size, uint16 queue_idx);
void virtqueue_activate(struct PCIDevice *device, struct virtqueue *vq);
void virtio_pci_reset(struct PCIDevice *device);
void virtio_pci_set_status(struct PCIDevice *device, uint8 status);
uint8 virtio_pci_get_status(struct PCIDevice *device);
uint32 virtio_pci_get_features(struct PCIDevice *device);
void virtio_pci_set_features(struct PCIDevice *device, uint32 features);
uint16 virtio_pci_get_queue_size(struct PCIDevice *device, uint16 queue_idx);
uint8 virtio_pci_get_isr(struct PCIDevice *device);
void virtio_pci_free_device(struct PCIDevice *device);

static void dma_complete_io(struct IOStdReq *ior, BOOL from_device) {
    ULONG dma_len;
    ULONG dma_flags;

    if (!ior || !ior->io_Data || (ior->io_Length == 0)) {
        return;
    }

    dma_len = ior->io_Length;
    dma_flags = from_device ? 0 : (DMAF_ReadFromRAM | DMAF_NoModify);
    IExec->CachePostDMA(ior->io_Data, &dma_len, dma_flags);
}

static void abort_pending_request(struct IORequest **slots, uint16 count, struct IORequest *ior) {
    uint16 i;

    for (i = 0; i < count; i++) {
        if (slots[i] == ior) {
            slots[i] = NULL;
            ior->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg(&ior->io_Message);
            return;
        }
    }
}

static void abort_all_pending_requests(struct IORequest **slots, uint16 count) {
    uint16 i;

    for (i = 0; i < count; i++) {
        struct IORequest *ior = slots[i];

        if (ior) {
            slots[i] = NULL;
            ior->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg(&ior->io_Message);
        }
    }
}

/* Funzione ISR (Interrupt Service Routine) */
uint32 APICALL virtio_isr(struct Interrupt *intr, APTR data) {
    struct VirtioBase *base = (struct VirtioBase *)data;
    uint8 isr_status;

    /* 1. Leggiamo lo stato del registro ISR di VirtIO */
    isr_status = virtio_pci_get_isr(base->pci_Dev);

    /* 2. Se non è impostato il bit di interrupt, non è un nostro evento */
    if (!(isr_status & VIRTIO_ISR_QUEUE_INTERRUPT)) {
        return 0;
    }

    /* 3. Se abbiamo nuovi dati, segnaliamo il task principale del driver */
    if (base->dev_Task) {
        IExec->Signal(base->dev_Task, (1 << base->sig_bit));
    }

    return 1;
}

/* Funzione del Task di Background */
void DeviceTaskFunc(uint32 base_addr) {
    struct VirtioBase *base = (struct VirtioBase *)base_addr;
    uint32 wait_mask = (1 << base->sig_bit) | SIGBREAKF_CTRL_F;

    while (1) {
        uint32 signals = IExec->Wait(wait_mask);

        /* Se riceviamo il segnale di chiusura (CTRL-F), usciamo */
        if (signals & SIGBREAKF_CTRL_F) break;

        /* Scansione RX (Ricezione dati) */
        uint32 len = 0;
        int16 idx;
        while ((idx = virtqueue_get_finished(base->rx_queue, &len)) >= 0) {
            struct IOStdReq *ior = (struct IOStdReq *)base->rx_reqs[idx];
            if (ior) {
                dma_complete_io(ior, TRUE);
                ior->io_Actual = len;
                ior->io_Error = 0;
                base->rx_reqs[idx] = NULL;
                IExec->ReplyMsg(&ior->io_Message);
            }
        }

        /* Scansione TX (Invio completato) */
        while ((idx = virtqueue_get_finished(base->tx_queue, &len)) >= 0) {
            struct IOStdReq *ior = (struct IOStdReq *)base->tx_reqs[idx];
            if (ior) {
                dma_complete_io(ior, FALSE);
                ior->io_Actual = len;
                ior->io_Error = 0;
                base->tx_reqs[idx] = NULL;
                IExec->ReplyMsg(&ior->io_Message);
            }
        }
    }
    
    /* Fine del task */
    base->dev_Task = NULL;
    if (base->task_owner && (base->task_done_sig_bit != 0xFFFFFFFF)) {
        IExec->Signal(base->task_owner, (1UL << base->task_done_sig_bit));
    }
}

/* Definizione globale dell'interfaccia Exec */
struct ExecIFace *IExec = NULL;

/* Funzioni di gestione Libreria */

struct Library * APICALL LibInit(struct Library *library, APTR seglist, struct Interface *exec) {
    struct VirtioBase *base = (struct VirtioBase *)library;

    /* Inizializziamo l'interfaccia Exec globale */
    IExec = (struct ExecIFace *)exec;
    IMMU = (struct MMUIFace *)IExec->GetInterface((struct Library *)IExec->Data.LibBase, (STRPTR)"mmu", 1, NULL);

    /* Apriamo la dos.library per IDOS->Delay */
    DosBase = IExec->OpenLibrary((STRPTR)"dos.library", 52);
    if (DosBase) {
        IDOS = (struct DOSIFace *)IExec->GetInterface(DosBase, (STRPTR)"main", 1, NULL);
    }

    /* Inizializzazione base del device */
    base->lib_Base.lib_Node.ln_Type = NT_DEVICE;
    base->lib_Base.lib_Node.ln_Name = (STRPTR)DEVICE_NAME;
    base->lib_Base.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib_Base.lib_Version = DEVICE_VERSION;
    base->lib_Base.lib_Revision = DEVICE_REVISION;
    base->lib_Base.lib_IdString = (STRPTR)DEVICE_ID_STRING;
    base->seg_List = seglist;
    
    /* Inizializzazione interfacce */
    base->lib_manager_iface.Data.LibBase = (struct Library *)base;
    base->device_manager_iface.Data.LibBase = (struct Library *)base;

    base->pci_Dev = NULL;
    base->rx_queue = NULL;
    base->tx_queue = NULL;
    base->dev_Task = NULL;
    base->task_owner = NULL;
    base->irq_Num = 0xFFFFFFFF;
    base->sig_bit = 0xFFFFFFFF;
    base->task_done_sig_bit = 0xFFFFFFFF;
    
    /* Configurazione nodo Interrupt */
    base->isr_Node.is_Node.ln_Type = NT_INTERRUPT;
    base->isr_Node.is_Node.ln_Pri = 0;
    base->isr_Node.is_Node.ln_Name = (STRPTR)DEVICE_NAME;
    base->isr_Node.is_Data = base;
    base->isr_Node.is_Code = (VOID (*)())virtio_isr;

    return (struct Library *)base;
}

uint32 APICALL LibObtain(struct LibraryManagerInterface *Self) {
    return Self->Data.RefCount++;
}

uint32 APICALL LibRelease(struct LibraryManagerInterface *Self) {
    return Self->Data.RefCount--;
}

struct Library * APICALL LibOpen(struct LibraryManagerInterface *Self, uint32 version) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;
    base->lib_Base.lib_OpenCnt++;
    base->lib_Base.lib_Flags &= ~LIBF_DELEXP;
    return (struct Library *)base;
}

APTR APICALL LibClose(struct LibraryManagerInterface *Self) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;
    base->lib_Base.lib_OpenCnt--;
    return NULL;
}

APTR APICALL LibExpunge(struct LibraryManagerInterface *Self) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;
    APTR seglist = NULL;

    if (base->lib_Base.lib_OpenCnt == 0) {
        /* Rimozione finale della libreria dalla memoria */
        seglist = base->seg_List;
        IExec->Remove(&base->lib_Base.lib_Node);

        /* Rilascio IDOS e chiusura libreria DOS */
        if (IMMU) IExec->DropInterface((struct Interface *)IMMU);
        if (IDOS) IExec->DropInterface((struct Interface *)IDOS);
        if (DosBase) IExec->CloseLibrary(DosBase);

        IExec->FreeVec(base);
        return seglist;
    }
    
    base->lib_Base.lib_Flags |= LIBF_DELEXP;
    return NULL;
}

struct Interface * APICALL LibGetInterface(struct LibraryManagerInterface *Self, STRPTR name, uint32 version, struct TagItem *taglist) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;

    /* Usiamo una comparazione sicura per il nome dell'interfaccia */
    if (name) {
        if (streq_nocase(name, (STRPTR)"main")) {
            return (struct Interface *)&base->lib_manager_iface;
        }
        if (streq_nocase(name, (STRPTR)"device")) {
            return (struct Interface *)&base->device_manager_iface;
        }
    }
    return NULL;
}

/* Funzioni core del Device */

VOID APICALL DevOpen(struct DeviceManagerInterface *Self, struct IORequest *ior, uint32 unit, uint32 flags) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;

    /* Scansione PCI se non ancora inizializzato */
    if (!base->pci_Dev) {
        int32 open_ok = 0;
        base->pci_Dev = find_virtio_serial_device();
        if (!base->pci_Dev) {
            ior->io_Error = IOERR_OPENFAIL;
            return;
        }

        /* Sequenza di Inizializzazione VirtIO (Legacy) */
        virtio_pci_reset(base->pci_Dev);
        virtio_pci_set_status(base->pci_Dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
        
        /* Negoziazione feature (Mock per ora) */
        base->features = virtio_pci_get_features(base->pci_Dev);
        virtio_pci_set_features(base->pci_Dev, 0);

        /* Allocazione VirtQueues (0: RX, 1: TX) */
        uint16 rx_size = virtio_pci_get_queue_size(base->pci_Dev, 0);
        uint16 tx_size = virtio_pci_get_queue_size(base->pci_Dev, 1);

        if (rx_size > VIRTIO_MAX_QUEUE_DESCRIPTORS) rx_size = VIRTIO_MAX_QUEUE_DESCRIPTORS;
        if (tx_size > VIRTIO_MAX_QUEUE_DESCRIPTORS) tx_size = VIRTIO_MAX_QUEUE_DESCRIPTORS;

        if ((rx_size == 0) || (tx_size == 0)) {
            ior->io_Error = IOERR_OPENFAIL;
            virtio_pci_set_status(base->pci_Dev, VIRTIO_STATUS_FAILED);
            virtio_pci_free_device(base->pci_Dev);
            base->pci_Dev = NULL;
            return;
        }

        base->rx_queue = virtqueue_alloc(rx_size, 0);
        base->tx_queue = virtqueue_alloc(tx_size, 1);
        
        if (base->rx_queue && base->tx_queue) {
            virtqueue_activate(base->pci_Dev, base->rx_queue);
            virtqueue_activate(base->pci_Dev, base->tx_queue);

            /* Registrazione ISR nel sistema AmigaOS */
            base->irq_Num = base->pci_Dev->MapInterrupt();
            if (base->irq_Num != 0xFFFFFFFF) {
                /* Allocazione di un segnale unico per il task */
                base->sig_bit = IExec->AllocSignal(-1);
                base->task_done_sig_bit = IExec->AllocSignal(-1);

                if ((base->sig_bit != 0xFFFFFFFF) && (base->task_done_sig_bit != 0xFFFFFFFF)) {
                    base->task_owner = IExec->FindTask(NULL);

                    base->dev_Task = IExec->CreateTaskTags(
                        (STRPTR)"virtio-serial helper",
                        0,
                        (APTR)DeviceTaskFunc,
                        8192,
                        AT_Param1, (uint32)base,
                        TAG_DONE
                    );

                    if (base->dev_Task && IExec->AddIntServer(base->irq_Num, &base->isr_Node)) {
                        virtio_pci_set_status(base->pci_Dev, virtio_pci_get_status(base->pci_Dev) | VIRTIO_STATUS_DRIVER_OK);
                        open_ok = 1;
                    } else {
                        if (base->dev_Task) {
                            IExec->DeleteTask(base->dev_Task);
                            base->dev_Task = NULL;
                        }
                    }
                }
            }
        } else {
            if (base->rx_queue) {
                virtqueue_free(base->rx_queue);
                base->rx_queue = NULL;
            }
            if (base->tx_queue) {
                virtqueue_free(base->tx_queue);
                base->tx_queue = NULL;
            }
            virtio_pci_set_status(base->pci_Dev, VIRTIO_STATUS_FAILED);
            virtio_pci_free_device(base->pci_Dev);
            base->pci_Dev = NULL;
            ior->io_Error = IOERR_OPENFAIL;
            return;
        }

        if (!open_ok) {
            if (base->rx_queue) {
                virtqueue_free(base->rx_queue);
                base->rx_queue = NULL;
            }
            if (base->tx_queue) {
                virtqueue_free(base->tx_queue);
                base->tx_queue = NULL;
            }
            if (base->sig_bit != 0xFFFFFFFF) {
                IExec->FreeSignal((BYTE)base->sig_bit);
                base->sig_bit = 0xFFFFFFFF;
            }
            if (base->task_done_sig_bit != 0xFFFFFFFF) {
                IExec->FreeSignal((BYTE)base->task_done_sig_bit);
                base->task_done_sig_bit = 0xFFFFFFFF;
            }
            base->task_owner = NULL;
            virtio_pci_set_status(base->pci_Dev, VIRTIO_STATUS_FAILED);
            virtio_pci_free_device(base->pci_Dev);
            base->pci_Dev = NULL;
            ior->io_Error = IOERR_OPENFAIL;
            return;
        }
    }

    base->lib_Base.lib_OpenCnt++;
    ior->io_Error = 0;
}

APTR APICALL DevClose(struct DeviceManagerInterface *Self, struct IORequest *ior) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;
    
    base->lib_Base.lib_OpenCnt--;

    /* Se non ci sono più utenti, puliamo l'hardware */
    if (base->lib_Base.lib_OpenCnt == 0) {
        /* 1. Rimuoviamo la ISR */
        if (base->irq_Num != 0xFFFFFFFF) {
            IExec->RemIntServer(base->irq_Num, &base->isr_Node);
            base->irq_Num = 0xFFFFFFFF;
        }

        /* 2. Fermiamo il Task di Background */
        if (base->dev_Task) {
            struct Task *caller = IExec->FindTask(NULL);
            IExec->Signal(base->dev_Task, SIGBREAKF_CTRL_F);
            if ((caller == base->task_owner) && (base->task_done_sig_bit != 0xFFFFFFFF)) {
                IExec->Wait(1UL << base->task_done_sig_bit);
            } else if (IDOS) {
                IDOS->Delay(2);
            }
        }

        abort_all_pending_requests(base->rx_reqs, VIRTIO_MAX_QUEUE_DESCRIPTORS);
        abort_all_pending_requests(base->tx_reqs, VIRTIO_MAX_QUEUE_DESCRIPTORS);

        /* 3. Reset hardware VirtIO */
        if (base->pci_Dev) {
            virtio_pci_set_status(base->pci_Dev, 0); /* Reset */
            virtio_pci_reset(base->pci_Dev);
            virtio_pci_free_device(base->pci_Dev);
            base->pci_Dev = NULL;
        }

        /* 4. Liberiamo le code */
        if (base->rx_queue) {
            virtqueue_free(base->rx_queue);
            base->rx_queue = NULL;
        }
        if (base->tx_queue) {
            virtqueue_free(base->tx_queue);
            base->tx_queue = NULL;
        }
        
        /* Libera segnale */
        if (base->sig_bit != 0xFFFFFFFF) {
            IExec->FreeSignal(base->sig_bit);
            base->sig_bit = 0xFFFFFFFF;
        }
        if (base->task_done_sig_bit != 0xFFFFFFFF) {
            IExec->FreeSignal(base->task_done_sig_bit);
            base->task_done_sig_bit = 0xFFFFFFFF;
        }
        base->task_owner = NULL;
    }

    return NULL;
}

VOID APICALL DevBeginIO(struct DeviceManagerInterface *Self, struct IORequest *ior) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;
    struct IOStdReq *std_ior = (struct IOStdReq *)ior;
    int16 desc_idx = -1;

    if (!base->pci_Dev || !base->rx_queue || !base->tx_queue) {
        ior->io_Error = IOERR_OPENFAIL;
        IExec->ReplyMsg(&ior->io_Message);
        return;
    }

    /* Gestione CMD_READ / CMD_WRITE */
    switch (ior->io_Command) {
        case CMD_READ:
            /* Forniamo un buffer alla coda RX per ricevere dati */
            desc_idx = virtqueue_add_buffer(base->rx_queue, (uint32)std_ior->io_Data, std_ior->io_Length, VIRTQ_DESC_F_WRITE);
            if (desc_idx >= 0) {
                base->rx_reqs[desc_idx] = ior;
                ior->io_Flags &= ~IOF_QUICK;
                virtio_pci_notify(base->pci_Dev, 0); 
            } else {
                ior->io_Error = IOERR_OPENFAIL;
                IExec->ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_WRITE:
            /* Inseriamo i dati nella coda TX */
            desc_idx = virtqueue_add_buffer(base->tx_queue, (uint32)std_ior->io_Data, std_ior->io_Length, 0);
            if (desc_idx >= 0) {
                base->tx_reqs[desc_idx] = ior;
                ior->io_Flags &= ~IOF_QUICK;
                virtio_pci_notify(base->pci_Dev, 1);
            } else {
                ior->io_Error = IOERR_OPENFAIL;
                IExec->ReplyMsg(&ior->io_Message);
            }
            break;

        default:
            ior->io_Error = IOERR_NOCMD;
            IExec->ReplyMsg(&ior->io_Message);
            break;
    }
}

VOID APICALL DevAbortIO(struct DeviceManagerInterface *Self, struct IORequest *ior) {
    struct VirtioBase *base = (struct VirtioBase *)Self->Data.LibBase;

    abort_pending_request(base->rx_reqs, VIRTIO_MAX_QUEUE_DESCRIPTORS, ior);
    abort_pending_request(base->tx_reqs, VIRTIO_MAX_QUEUE_DESCRIPTORS, ior);
}
