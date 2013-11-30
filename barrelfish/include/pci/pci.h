/**
 * \file
 * \brief PCI configuration library.
 */

/*
 * Copyright (c) 2007, 2008, 2009, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef PCI_H
#define PCI_H

#include <barrelfish/inthandler.h>
#include <pci/mem.h>
#include <pci/devids.h>

typedef void (*pci_driver_init_fn)(struct device_mem *bar_info,
                                   int nr_mapped_bars);
typedef void (*legacy_driver_init_fn)(void);

errval_t pci_register_driver_noirq(pci_driver_init_fn init_func, uint32_t class,
                                   uint32_t subclass, uint32_t prog_if,
                                   uint32_t vendor, uint32_t device,
                                   uint32_t bus, uint32_t dev, uint32_t fun);

errval_t pci_register_driver_irq(pci_driver_init_fn init_func, uint32_t class,
                                 uint32_t subclass, uint32_t prog_if,
                                 uint32_t vendor, uint32_t device,
                                 uint32_t bus, uint32_t dev, uint32_t fun,
                                 interrupt_handler_fn handler, void *handler_arg);

errval_t pci_register_legacy_driver_irq(legacy_driver_init_fn init_func,
                                        uint16_t iomin, uint16_t iomax, int irq,
                                        interrupt_handler_fn handler,
                                        void *handler_arg);

errval_t pci_setup_inthandler(interrupt_handler_fn handler, void *handler_arg,
                              int *ret_vector);


errval_t pci_client_connect(void);

#endif
