#ifndef VIRTIO_SERIAL_H
#define VIRTIO_SERIAL_H

#include <exec/types.h>

/* VirtIO Serial Configuration Space (BAR0 + Offset 20h in Legacy) */
struct virtio_serial_config {
    uint32 cols;
    uint32 rows;
    uint32 max_nr_ports;
};

/* VirtIO Serial Feature Bits */
#define VIRTIO_CONSOLE_F_SIZE       0  /* Supporta colonne e righe */
#define VIRTIO_CONSOLE_F_MULTIPORT  1  /* Supporta più porte e una coda di controllo */
#define VIRTIO_CONSOLE_F_EMERG_WRITE 2 /* Supporta scrittura emergenza in config space */

/* VirtIO Serial Control Message Types */
#define VIRTIO_CONSOLE_DEVICE_READY     0
#define VIRTIO_CONSOLE_PORT_ADD         1
#define VIRTIO_CONSOLE_PORT_REMOVE      2
#define VIRTIO_CONSOLE_PORT_READY       3
#define VIRTIO_CONSOLE_CONSOLE_PORT     4
#define VIRTIO_CONSOLE_RESIZE           5
#define VIRTIO_CONSOLE_PORT_OPEN        6
#define VIRTIO_CONSOLE_PORT_NAME        7

#endif /* VIRTIO_SERIAL_H */
