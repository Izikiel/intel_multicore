# Copyright (c) 2009-2012 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Ali Saidi
#          Gabe Black
#          William Wang

from m5.params import *
from m5.proxy import *
from Device import BasicPioDevice, PioDevice, IsaFake, BadAddr, DmaDevice
from Pci import PciConfigAll
from Ethernet import NSGigE, IGbE_e1000, IGbE_igb
from Ide import *
from Platform import Platform
from Terminal import Terminal
from Uart import Uart
from SimpleMemory import SimpleMemory

class AmbaDevice(BasicPioDevice):
    type = 'AmbaDevice'
    abstract = True
    amba_id = Param.UInt32("ID of AMBA device for kernel detection")

class AmbaIntDevice(AmbaDevice):
    type = 'AmbaIntDevice'
    abstract = True
    gic = Param.Gic(Parent.any, "Gic to use for interrupting")
    int_num = Param.UInt32("Interrupt number that connects to GIC")
    int_delay = Param.Latency("100ns",
            "Time between action and interrupt generation by device")

class AmbaDmaDevice(DmaDevice):
    type = 'AmbaDmaDevice'
    abstract = True
    pio_addr = Param.Addr("Address for AMBA slave interface")
    pio_latency = Param.Latency("10ns", "Time between action and write/read result by AMBA DMA Device")
    gic = Param.Gic(Parent.any, "Gic to use for interrupting")
    int_num = Param.UInt32("Interrupt number that connects to GIC")
    amba_id = Param.UInt32("ID of AMBA device for kernel detection")

class A9SCU(BasicPioDevice):
    type = 'A9SCU'

class RealViewCtrl(BasicPioDevice):
    type = 'RealViewCtrl'
    proc_id0 = Param.UInt32(0x0C000000, "Processor ID, SYS_PROCID")
    proc_id1 = Param.UInt32(0x0C000222, "Processor ID, SYS_PROCID1")
    idreg = Param.UInt32(0x00000000, "ID Register, SYS_ID")

class Gic(PioDevice):
    type = 'Gic'
    platform = Param.Platform(Parent.any, "Platform this device is part of.")
    dist_addr = Param.Addr(0x1f001000, "Address for distributor")
    cpu_addr = Param.Addr(0x1f000100, "Address for cpu")
    dist_pio_delay = Param.Latency('10ns', "Delay for PIO r/w to distributor")
    cpu_pio_delay = Param.Latency('10ns', "Delay for PIO r/w to cpu interface")
    int_latency = Param.Latency('10ns', "Delay for interrupt to get to CPU")
    it_lines = Param.UInt32(128, "Number of interrupt lines supported (max = 1020)")

class AmbaFake(AmbaDevice):
    type = 'AmbaFake'
    ignore_access = Param.Bool(False, "Ignore reads/writes to this device, (e.g. IsaFake + AMBA)")
    amba_id = 0;

class Pl011(Uart):
    type = 'Pl011'
    gic = Param.Gic(Parent.any, "Gic to use for interrupting")
    int_num = Param.UInt32("Interrupt number that connects to GIC")
    end_on_eot = Param.Bool(False, "End the simulation when a EOT is received on the UART")
    int_delay = Param.Latency("100ns", "Time between action and interrupt generation by UART")

class Sp804(AmbaDevice):
    type = 'Sp804'
    gic = Param.Gic(Parent.any, "Gic to use for interrupting")
    int_num0 = Param.UInt32("Interrupt number that connects to GIC")
    clock0 = Param.Clock('1MHz', "Clock speed of the input")
    int_num1 = Param.UInt32("Interrupt number that connects to GIC")
    clock1 = Param.Clock('1MHz', "Clock speed of the input")
    amba_id = 0x00141804

class CpuLocalTimer(BasicPioDevice):
    type = 'CpuLocalTimer'
    gic = Param.Gic(Parent.any, "Gic to use for interrupting")
    int_num_timer = Param.UInt32("Interrrupt number used per-cpu to GIC")
    int_num_watchdog = Param.UInt32("Interrupt number for per-cpu watchdog to GIC")
    clock = Param.Clock('1GHz', "Clock speed at which the timer counts")

class PL031(AmbaIntDevice):
    type = 'PL031'
    time = Param.Time('01/01/2009', "System time to use ('Now' for actual time)")
    amba_id = 0x00341031

class Pl050(AmbaIntDevice):
    type = 'Pl050'
    vnc = Param.VncServer(Parent.any, "Vnc server for remote frame buffer display")
    is_mouse = Param.Bool(False, "Is this interface a mouse, if not a keyboard")
    int_delay = '1us'
    amba_id = 0x00141050

class Pl111(AmbaDmaDevice):
    type = 'Pl111'
    clock = Param.Clock('24MHz', "Clock speed of the input")
    vnc   = Param.VncServer(Parent.any, "Vnc server for remote frame buffer display")
    amba_id = 0x00141111

class RealView(Platform):
    type = 'RealView'
    system = Param.System(Parent.any, "system")
    pci_cfg_base = Param.Addr(0, "Base address of PCI Configuraiton Space")
    mem_start_addr = Param.Addr(0, "Start address of main memory")
    max_mem_size = Param.Addr('256MB', "Maximum amount of RAM supported by platform")

    def setupBootLoader(self, mem_bus, cur_sys, loc):
        self.nvmem = SimpleMemory(range = AddrRange(Addr('2GB'),
                                                    size = '64MB'),
                                  zero = True)
        self.nvmem.port = mem_bus.master
        cur_sys.boot_loader = loc('boot.arm')


# Reference for memory map and interrupt number
# RealView Platform Baseboard Explore for Cortex-A9 User Guide(ARM DUI 0440A)
# Chapter 4: Programmer's Reference
class RealViewPBX(RealView):
    uart = Pl011(pio_addr=0x10009000, int_num=44)
    realview_io = RealViewCtrl(pio_addr=0x10000000)
    gic = Gic()
    timer0 = Sp804(int_num0=36, int_num1=36, pio_addr=0x10011000)
    timer1 = Sp804(int_num0=37, int_num1=37, pio_addr=0x10012000)
    local_cpu_timer = CpuLocalTimer(int_num_timer=29, int_num_watchdog=30, pio_addr=0x1f000600)
    clcd = Pl111(pio_addr=0x10020000, int_num=55)
    kmi0   = Pl050(pio_addr=0x10006000, int_num=52)
    kmi1   = Pl050(pio_addr=0x10007000, int_num=53, is_mouse=True)
    a9scu  = A9SCU(pio_addr=0x1f000000)
    cf_ctrl = IdeController(disks=[], pci_func=0, pci_dev=7, pci_bus=2,
                            io_shift = 1, ctrl_offset = 2, Command = 0x1,
                            BAR0 = 0x18000000, BAR0Size = '16B',
                            BAR1 = 0x18000100, BAR1Size = '1B',
                            BAR0LegacyIO = True, BAR1LegacyIO = True)


    l2x0_fake     = IsaFake(pio_addr=0x1f002000, pio_size=0xfff)
    flash_fake    = IsaFake(pio_addr=0x40000000, pio_size=0x20000000,
                            fake_mem=True)
    dmac_fake     = AmbaFake(pio_addr=0x10030000)
    uart1_fake    = AmbaFake(pio_addr=0x1000a000)
    uart2_fake    = AmbaFake(pio_addr=0x1000b000)
    uart3_fake    = AmbaFake(pio_addr=0x1000c000)
    smc_fake      = AmbaFake(pio_addr=0x100e1000)
    sp810_fake    = AmbaFake(pio_addr=0x10001000, ignore_access=True)
    watchdog_fake = AmbaFake(pio_addr=0x10010000)
    gpio0_fake    = AmbaFake(pio_addr=0x10013000)
    gpio1_fake    = AmbaFake(pio_addr=0x10014000)
    gpio2_fake    = AmbaFake(pio_addr=0x10015000)
    ssp_fake      = AmbaFake(pio_addr=0x1000d000)
    sci_fake      = AmbaFake(pio_addr=0x1000e000)
    aaci_fake     = AmbaFake(pio_addr=0x10004000)
    mmc_fake      = AmbaFake(pio_addr=0x10005000)
    rtc           = PL031(pio_addr=0x10017000, int_num=42)


    # Attach I/O devices that are on chip and also set the appropriate
    # ranges for the bridge
    def attachOnChipIO(self, bus, bridge):
       self.gic.pio = bus.master
       self.l2x0_fake.pio = bus.master
       self.a9scu.pio = bus.master
       self.local_cpu_timer.pio = bus.master
       # Bridge ranges based on excluding what is part of on-chip I/O
       # (gic, l2x0, a9scu, local_cpu_timer)
       bridge.ranges = [AddrRange(self.realview_io.pio_addr,
                                  self.a9scu.pio_addr - 1),
                        AddrRange(self.flash_fake.pio_addr,
                                  self.flash_fake.pio_addr + \
                                  self.flash_fake.pio_size - 1)]

    # Attach I/O devices to specified bus object.  Can't do this
    # earlier, since the bus object itself is typically defined at the
    # System level.
    def attachIO(self, bus):
       self.uart.pio          = bus.master
       self.realview_io.pio   = bus.master
       self.timer0.pio        = bus.master
       self.timer1.pio        = bus.master
       self.clcd.pio          = bus.master
       self.clcd.dma          = bus.slave
       self.kmi0.pio          = bus.master
       self.kmi1.pio          = bus.master
       self.cf_ctrl.pio       = bus.master
       self.cf_ctrl.config    = bus.master
       self.cf_ctrl.dma       = bus.slave
       self.dmac_fake.pio     = bus.master
       self.uart1_fake.pio    = bus.master
       self.uart2_fake.pio    = bus.master
       self.uart3_fake.pio    = bus.master
       self.smc_fake.pio      = bus.master
       self.sp810_fake.pio    = bus.master
       self.watchdog_fake.pio = bus.master
       self.gpio0_fake.pio    = bus.master
       self.gpio1_fake.pio    = bus.master
       self.gpio2_fake.pio    = bus.master
       self.ssp_fake.pio      = bus.master
       self.sci_fake.pio      = bus.master
       self.aaci_fake.pio     = bus.master
       self.mmc_fake.pio      = bus.master
       self.rtc.pio           = bus.master
       self.flash_fake.pio    = bus.master

# Reference for memory map and interrupt number
# RealView Emulation Baseboard User Guide (ARM DUI 0143B)
# Chapter 4: Programmer's Reference
class RealViewEB(RealView):
    uart = Pl011(pio_addr=0x10009000, int_num=44)
    realview_io = RealViewCtrl(pio_addr=0x10000000)
    gic = Gic(dist_addr=0x10041000, cpu_addr=0x10040000)
    timer0 = Sp804(int_num0=36, int_num1=36, pio_addr=0x10011000)
    timer1 = Sp804(int_num0=37, int_num1=37, pio_addr=0x10012000)
    clcd   = Pl111(pio_addr=0x10020000, int_num=23)
    kmi0   = Pl050(pio_addr=0x10006000, int_num=20)
    kmi1   = Pl050(pio_addr=0x10007000, int_num=21, is_mouse=True)

    l2x0_fake     = IsaFake(pio_addr=0x1f002000, pio_size=0xfff, warn_access="1")
    flash_fake    = IsaFake(pio_addr=0x40000000, pio_size=0x20000000-1,
                            fake_mem=True)
    dmac_fake     = AmbaFake(pio_addr=0x10030000)
    uart1_fake    = AmbaFake(pio_addr=0x1000a000)
    uart2_fake    = AmbaFake(pio_addr=0x1000b000)
    uart3_fake    = AmbaFake(pio_addr=0x1000c000)
    smcreg_fake   = IsaFake(pio_addr=0x10080000, pio_size=0x10000-1)
    smc_fake      = AmbaFake(pio_addr=0x100e1000)
    sp810_fake    = AmbaFake(pio_addr=0x10001000, ignore_access=True)
    watchdog_fake = AmbaFake(pio_addr=0x10010000)
    gpio0_fake    = AmbaFake(pio_addr=0x10013000)
    gpio1_fake    = AmbaFake(pio_addr=0x10014000)
    gpio2_fake    = AmbaFake(pio_addr=0x10015000)
    ssp_fake      = AmbaFake(pio_addr=0x1000d000)
    sci_fake      = AmbaFake(pio_addr=0x1000e000)
    aaci_fake     = AmbaFake(pio_addr=0x10004000)
    mmc_fake      = AmbaFake(pio_addr=0x10005000)
    rtc_fake      = AmbaFake(pio_addr=0x10017000, amba_id=0x41031)



    # Attach I/O devices that are on chip and also set the appropriate
    # ranges for the bridge
    def attachOnChipIO(self, bus, bridge):
       self.gic.pio = bus.master
       self.l2x0_fake.pio = bus.master
       # Bridge ranges based on excluding what is part of on-chip I/O
       # (gic, l2x0)
       bridge.ranges = [AddrRange(self.realview_io.pio_addr,
                                  self.gic.cpu_addr - 1),
                        AddrRange(self.flash_fake.pio_addr, Addr.max)]

    # Attach I/O devices to specified bus object.  Can't do this
    # earlier, since the bus object itself is typically defined at the
    # System level.
    def attachIO(self, bus):
       self.uart.pio          = bus.master
       self.realview_io.pio   = bus.master
       self.timer0.pio        = bus.master
       self.timer1.pio        = bus.master
       self.clcd.pio          = bus.master
       self.clcd.dma          = bus.slave
       self.kmi0.pio          = bus.master
       self.kmi1.pio          = bus.master
       self.dmac_fake.pio     = bus.master
       self.uart1_fake.pio    = bus.master
       self.uart2_fake.pio    = bus.master
       self.uart3_fake.pio    = bus.master
       self.smc_fake.pio      = bus.master
       self.sp810_fake.pio    = bus.master
       self.watchdog_fake.pio = bus.master
       self.gpio0_fake.pio    = bus.master
       self.gpio1_fake.pio    = bus.master
       self.gpio2_fake.pio    = bus.master
       self.ssp_fake.pio      = bus.master
       self.sci_fake.pio      = bus.master
       self.aaci_fake.pio     = bus.master
       self.mmc_fake.pio      = bus.master
       self.rtc_fake.pio      = bus.master
       self.flash_fake.pio    = bus.master
       self.smcreg_fake.pio   = bus.master

class VExpress_ELT(RealView):
    max_mem_size = '2GB'
    pci_cfg_base = 0xD0000000
    uart0 = Pl011(pio_addr=0xE0009000, int_num=42)
    uart1 = Pl011(pio_addr=0xE000A000, int_num=43)
    uart = Pl011(pio_addr=0xFF009000, int_num=121)
    realview_io = RealViewCtrl(proc_id0=0x0C000222, pio_addr=0xFF000000)
    gic = Gic(dist_addr=0xE0201000, cpu_addr=0xE0200100)
    local_cpu_timer = CpuLocalTimer(int_num_timer=29, int_num_watchdog=30, pio_addr=0xE0200600)
    v2m_timer0 = Sp804(int_num0=120, int_num1=120, pio_addr=0xFF011000)
    v2m_timer1 = Sp804(int_num0=121, int_num1=121, pio_addr=0xFF012000)
    elba_timer0 = Sp804(int_num0=36, int_num1=36, pio_addr=0xE0011000, clock0='50MHz', clock1='50MHz')
    elba_timer1 = Sp804(int_num0=37, int_num1=37, pio_addr=0xE0012000, clock0='50MHz', clock1='50MHz')
    clcd   = Pl111(pio_addr=0xE0022000, int_num=46)   # CLCD interrupt no. unknown
    kmi0   = Pl050(pio_addr=0xFF006000, int_num=124)
    kmi1   = Pl050(pio_addr=0xFF007000, int_num=125)
    elba_kmi0   = Pl050(pio_addr=0xE0006000, int_num=52)
    elba_kmi1   = Pl050(pio_addr=0xE0007000, int_num=53)
    a9scu  = A9SCU(pio_addr=0xE0200000)
    cf_ctrl = IdeController(disks=[], pci_func=0, pci_dev=0, pci_bus=2,
                            io_shift = 2, ctrl_offset = 2, Command = 0x1,
                            BAR0 = 0xFF01A000, BAR0Size = '256B',
                            BAR1 = 0xFF01A100, BAR1Size = '4096B',
                            BAR0LegacyIO = True, BAR1LegacyIO = True)

    pciconfig = PciConfigAll()
    ethernet = IGbE_e1000(pci_bus=0, pci_dev=0, pci_func=0,
                          InterruptLine=1, InterruptPin=1)

    ide = IdeController(disks = [], pci_bus=0, pci_dev=1, pci_func=0,
                        InterruptLine=2, InterruptPin=2)

    l2x0_fake      = IsaFake(pio_addr=0xE0202000, pio_size=0xfff)
    dmac_fake      = AmbaFake(pio_addr=0xE0020000)
    uart2_fake     = AmbaFake(pio_addr=0xE000B000)
    uart3_fake     = AmbaFake(pio_addr=0xE000C000)
    smc_fake       = AmbaFake(pio_addr=0xEC000000)
    sp810_fake     = AmbaFake(pio_addr=0xFF001000, ignore_access=True)
    watchdog_fake  = AmbaFake(pio_addr=0xE0010000)
    aaci_fake      = AmbaFake(pio_addr=0xFF004000)
    elba_aaci_fake = AmbaFake(pio_addr=0xE0004000)
    mmc_fake       = AmbaFake(pio_addr=0xE0005000) # not sure if we need this
    rtc_fake       = AmbaFake(pio_addr=0xE0017000, amba_id=0x41031)
    spsc_fake      = IsaFake(pio_addr=0xE001B000, pio_size=0x2000)
    lan_fake       = IsaFake(pio_addr=0xFA000000, pio_size=0xffff)
    usb_fake       = IsaFake(pio_addr=0xFB000000, pio_size=0x1ffff)


    # Attach I/O devices that are on chip and also set the appropriate
    # ranges for the bridge
    def attachOnChipIO(self, bus, bridge):
       self.gic.pio = bus.master
       self.a9scu.pio = bus.master
       self.local_cpu_timer.pio = bus.master
       # Bridge ranges based on excluding what is part of on-chip I/O
       # (gic, a9scu)
       bridge.ranges = [AddrRange(self.pci_cfg_base, self.a9scu.pio_addr - 1),
                        AddrRange(self.l2x0_fake.pio_addr, Addr.max)]

    # Attach I/O devices to specified bus object.  Can't do this
    # earlier, since the bus object itself is typically defined at the
    # System level.
    def attachIO(self, bus):
       self.uart0.pio     		= bus.master
       self.uart1.pio     		= bus.master
       self.uart.pio            = bus.master
       self.realview_io.pio     = bus.master
       self.v2m_timer0.pio      = bus.master
       self.v2m_timer1.pio      = bus.master
       self.elba_timer0.pio     = bus.master
       self.elba_timer1.pio     = bus.master
       self.clcd.pio            = bus.master
       self.clcd.dma            = bus.slave
       self.kmi0.pio            = bus.master
       self.kmi1.pio            = bus.master
       self.elba_kmi0.pio       = bus.master
       self.elba_kmi1.pio       = bus.master
       self.cf_ctrl.pio         = bus.master
       self.cf_ctrl.config      = bus.master
       self.cf_ctrl.dma         = bus.slave
       self.ide.pio             = bus.master
       self.ide.config          = bus.master
       self.ide.dma             = bus.slave
       self.ethernet.pio        = bus.master
       self.ethernet.config     = bus.master
       self.ethernet.dma        = bus.slave
       self.pciconfig.pio       = bus.default
       bus.use_default_range    = True

       self.l2x0_fake.pio       = bus.master
       self.dmac_fake.pio       = bus.master
       self.uart2_fake.pio      = bus.master
       self.uart3_fake.pio      = bus.master
       self.smc_fake.pio        = bus.master
       self.sp810_fake.pio      = bus.master
       self.watchdog_fake.pio   = bus.master
       self.aaci_fake.pio       = bus.master
       self.elba_aaci_fake.pio  = bus.master
       self.mmc_fake.pio        = bus.master
       self.rtc_fake.pio        = bus.master
       self.spsc_fake.pio       = bus.master
       self.lan_fake.pio        = bus.master
       self.usb_fake.pio        = bus.master


class VExpress_EMM(RealView):
    mem_start_addr = '2GB'
    max_mem_size = '2GB'
    uart = Pl011(pio_addr=0x1c090000, int_num=37)
    realview_io = RealViewCtrl(proc_id0=0x14000000, proc_id1=0x14000000, pio_addr=0x1C010000)
    gic = Gic(dist_addr=0x2C001000, cpu_addr=0x2C002000)
    local_cpu_timer = CpuLocalTimer(int_num_timer=29, int_num_watchdog=30, pio_addr=0x2C080000)
    timer0 = Sp804(int_num0=34, int_num1=34, pio_addr=0x1C110000, clock0='50MHz', clock1='50MHz')
    timer1 = Sp804(int_num0=35, int_num1=35, pio_addr=0x1C120000, clock0='50MHz', clock1='50MHz')
    clcd   = Pl111(pio_addr=0x1c1f0000, int_num=46)
    kmi0   = Pl050(pio_addr=0x1c060000, int_num=44)
    kmi1   = Pl050(pio_addr=0x1c070000, int_num=45)
    cf_ctrl = IdeController(disks=[], pci_func=0, pci_dev=0, pci_bus=2,
                            io_shift = 2, ctrl_offset = 2, Command = 0x1,
                            BAR0 = 0x1C1A0000, BAR0Size = '256B',
                            BAR1 = 0x1C1A0100, BAR1Size = '4096B',
                            BAR0LegacyIO = True, BAR1LegacyIO = True)
    vram           = SimpleMemory(range = AddrRange(0x18000000, size='32MB'),
                                  zero = True)
    rtc            = PL031(pio_addr=0x1C170000, int_num=36)

    l2x0_fake      = IsaFake(pio_addr=0x2C100000, pio_size=0xfff)
    uart1_fake     = AmbaFake(pio_addr=0x1C0A0000)
    uart2_fake     = AmbaFake(pio_addr=0x1C0B0000)
    uart3_fake     = AmbaFake(pio_addr=0x1C0C0000)
    sp810_fake     = AmbaFake(pio_addr=0x1C020000, ignore_access=True)
    watchdog_fake  = AmbaFake(pio_addr=0x1C0F0000)
    aaci_fake      = AmbaFake(pio_addr=0x1C040000)
    lan_fake       = IsaFake(pio_addr=0x1A000000, pio_size=0xffff)
    usb_fake       = IsaFake(pio_addr=0x1B000000, pio_size=0x1ffff)
    mmc_fake       = AmbaFake(pio_addr=0x1c050000)

    def setupBootLoader(self, mem_bus, cur_sys, loc):
        self.nvmem = SimpleMemory(range = AddrRange(0, size = '64MB'),
                                  zero = True)
        self.nvmem.port = mem_bus.master
        cur_sys.boot_loader = loc('boot_emm.arm')
        cur_sys.atags_addr = 0x80000100

    # Attach I/O devices that are on chip and also set the appropriate
    # ranges for the bridge
    def attachOnChipIO(self, bus, bridge):
       self.gic.pio = bus.master
       self.local_cpu_timer.pio = bus.master
       # Bridge ranges based on excluding what is part of on-chip I/O
       # (gic, a9scu)
       bridge.ranges = [AddrRange(0x2F000000, size='16MB'),
                        AddrRange(0x30000000, size='256MB'),
                        AddrRange(0x18000000, size='64MB'),
                        AddrRange(0x1C000000, size='64MB')]

    # Attach I/O devices to specified bus object.  Can't do this
    # earlier, since the bus object itself is typically defined at the
    # System level.
    def attachIO(self, bus):
       self.uart.pio            = bus.master
       self.realview_io.pio     = bus.master
       self.timer0.pio          = bus.master
       self.timer1.pio          = bus.master
       self.clcd.pio            = bus.master
       self.clcd.dma            = bus.slave
       self.kmi0.pio            = bus.master
       self.kmi1.pio            = bus.master
       self.cf_ctrl.pio         = bus.master
       self.cf_ctrl.dma         = bus.slave
       self.cf_ctrl.config      = bus.master
       self.rtc.pio             = bus.master
       bus.use_default_range    = True
       self.vram.port           = bus.master

       self.l2x0_fake.pio       = bus.master
       self.uart1_fake.pio      = bus.master
       self.uart2_fake.pio      = bus.master
       self.uart3_fake.pio      = bus.master
       self.sp810_fake.pio      = bus.master
       self.watchdog_fake.pio   = bus.master
       self.aaci_fake.pio       = bus.master
       self.lan_fake.pio        = bus.master
       self.usb_fake.pio        = bus.master
       self.mmc_fake.pio        = bus.master

