#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include <exec/types.h>
#include <expansion/pci.h>

/* VirtIO PCI Vendor/Device IDs */
#define VIRTIO_VENDOR_ID          0x1AF4
#define VIRTIO_DEVICE_ID_SERIAL   0x1003  /* Legacy Serial */

/* VirtIO PCI Legacy Register Offsets (BAR0) */
#define VIRTIO_PCI_HOST_FEATURES  0x00  /* 32-bit R */
#define VIRTIO_PCI_GUEST_FEATURES 0x04  /* 32-bit RW */
#define VIRTIO_PCI_QUEUE_PFN      0x08  /* 32-bit RW */
#define VIRTIO_PCI_QUEUE_SIZE     0x0C  /* 16-bit R */
#define VIRTIO_PCI_QUEUE_SEL      0x0E  /* 16-bit RW */
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10  /* 16-bit RW */
#define VIRTIO_PCI_STATUS         0x12  /* 8-bit RW */
#define VIRTIO_PCI_ISR            0x13  /* 8-bit R */

/* VirtIO Status Bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

/* VirtIO ISR Bits */
#define VIRTIO_ISR_QUEUE_INTERRUPT 1
#define VIRTIO_ISR_CONFIG_CHANGE   2

/* VirtQueue Alignment */
#define VIRTQUEUE_ALIGN           4096

/* Prototipi funzioni in virtio_pci.c */
struct PCIDevice * find_virtio_serial_device();
uint8 virtio_pci_get_status(struct PCIDevice *device);
void virtio_pci_set_status(struct PCIDevice *device, uint8 status);
void virtio_pci_reset(struct PCIDevice *device);
uint32 virtio_pci_get_features(struct PCIDevice *device);
void virtio_pci_set_features(struct PCIDevice *device, uint32 features);
uint16 virtio_pci_get_queue_size(struct PCIDevice *device, uint16 queue_idx);
void virtio_pci_setup_queue(struct PCIDevice *device, uint16 queue_idx, uint32 pfn);
void virtio_pci_notify(struct PCIDevice *device, uint16 queue_idx);
void virtio_pci_free_device(struct PCIDevice *device);

#endif /* VIRTIO_PCI_H */
