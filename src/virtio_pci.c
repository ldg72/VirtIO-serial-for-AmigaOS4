#include <exec/types.h>
#include <exec/exec.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <interfaces/expansion.h>
#include <expansion/pci.h>

#include "virtio_pci.h"

/* Riferimento all'interfaccia Exec globale definita in main.c */
extern struct ExecIFace *IExec;

/* Trova il device VirtIO-serial sulla macchina */
struct PCIDevice * find_virtio_serial_device() {
    struct PCIDevice *device = NULL;
    struct Library *ExpansionBase = IExec->OpenLibrary("expansion.library", 52);
    
    if (ExpansionBase) {
        struct PCIIFace *IPCI = (struct PCIIFace *)IExec->GetInterface(ExpansionBase, "pci", 1, NULL);
        if (IPCI) {
            /* Cerchiamo il Vendor ID 0x1AF4 e il Device ID 0x1003 (Legacy) */
            device = IPCI->FindDeviceTags(
                FDT_VendorID, VIRTIO_VENDOR_ID,
                FDT_DeviceID, VIRTIO_DEVICE_ID_SERIAL,
                TAG_DONE
            );

            IExec->DropInterface((struct Interface *)IPCI);
        }
        IExec->CloseLibrary(ExpansionBase);
    }
    
    return device;
}

/* Inizializzazione del device VirtIO */
uint8 virtio_pci_get_status(struct PCIDevice *device) {
    return device->InByte(VIRTIO_PCI_STATUS);
}

void virtio_pci_set_status(struct PCIDevice *device, uint8 status) {
    device->OutByte(VIRTIO_PCI_STATUS, status);
}

void virtio_pci_reset(struct PCIDevice *device) {
    virtio_pci_set_status(device, 0);
}

/* Lettura feature dell'host */
uint32 virtio_pci_get_features(struct PCIDevice *device) {
    return device->InLong(VIRTIO_PCI_HOST_FEATURES);
}

/* Scrittura feature del guest */
void virtio_pci_set_features(struct PCIDevice *device, uint32 features) {
    device->OutLong(VIRTIO_PCI_GUEST_FEATURES, features);
}

uint16 virtio_pci_get_queue_size(struct PCIDevice *device, uint16 queue_idx) {
    device->OutWord(VIRTIO_PCI_QUEUE_SEL, queue_idx);
    return device->InWord(VIRTIO_PCI_QUEUE_SIZE);
}

/* Selezione e configurazione di una VirtQueue */
void virtio_pci_setup_queue(struct PCIDevice *device, uint16 queue_idx, uint32 pfn) {
    device->OutWord(VIRTIO_PCI_QUEUE_SEL, queue_idx);
    device->OutLong(VIRTIO_PCI_QUEUE_PFN, pfn);
}

/* Notifica all'host di nuovi dati in una coda */
void virtio_pci_notify(struct PCIDevice *device, uint16 queue_idx) {
    device->OutWord(VIRTIO_PCI_QUEUE_NOTIFY, queue_idx);
}

/* Lettura del registro ISR per confermare l'identità dell'interrupt */
uint8 virtio_pci_get_isr(struct PCIDevice *device) {
    return device->InByte(VIRTIO_PCI_ISR);
}

void virtio_pci_free_device(struct PCIDevice *device) {
    if (device) {
        device->Release();
    }
}
