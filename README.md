tapbr
=====

`tapbr` (tap bridge) is a DPDK-based, mirroring network bridge. In
order to build `tapbr` you need to have obtained and built DPDK first,
then you set the `RTE_SDK` and `RTE_TARGET` environment variable
appropriately and run `make`.

To see what command-line options are available, run `./tapbr --
--help`. The extra `--` is necessary because `tapbr` accepts two sets
of argument, the first set is passed to initialize DPDK's EAL, the
second set, separated by `--` is used by `tapbr` itself. You need to
have setup huge pages properly as instructed by the DPDK guides. You
will also need to run `tapbr` as root.

`tapbr` bridges two network interfaces, while mirroring the entire set
of traffic either to another interface, or to a number of DPDK
rings. If the latter option is used, another program can read packets
from these rings (obtained by `rte_ring_lookup`) and use them as
appropriate.

## Building

Set the `RTE_SDK` environment variable to where DPDK is built and
installed, and `RTE_TARGET` to your target environment (for example,
`x86_64-native-linuxapp-gcc`). Then go to the `tapbr` source directory
and run `make`. Aside from DPDK, you will also need libsystemd
development files because `tapbr` uses `sd-bus` to expose a dbus
interface.

## Usage

Make sure the Linux `uio` driver, as well as DPDK's `igb_uio` are
loaded. Also make sure you have reserved enough huge pages by
something like:

    # echo 64 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

Suppose you have three spare interfaces as shown by `dpdk_nic_bind`:

    $ dpdk_nic_bind --status

    Network devices using DPDK-compatible driver
    ============================================
    <none>

    Network devices using kernel driver
    ===================================
    0000:00:03.0 '82540EM Gigabit Ethernet Controller' if=eth0 drv=e1000 unused=igb_uio *Active*
    0000:00:07.0 '82545EM Gigabit Ethernet Controller (Copper)' if=eth1 drv=e1000 unused=igb_uio
    0000:00:08.0 '82545EM Gigabit Ethernet Controller (Copper)' if=eth1 drv=e1000 unused=igb_uio
    0000:00:09.0 '82545EM Gigabit Ethernet Controller (Copper)' if=eth1 drv=e1000 unused=igb_uio

    Other network devices
    =====================
    <none>

Bind them to DPDK's `igb_uio` driver:

    $ sudo ../dpdk-16.07/tools/dpdk-devbind.py --bind=igb_uio 0000:00:07.0
    $ sudo ../dpdk-16.07/tools/dpdk-devbind.py --bind=igb_uio 0000:00:08.0
    $ sudo ../dpdk-16.07/tools/dpdk-devbind.py --bind=igb_uio 0000:00:09.0
    $ dpdk_nic_bind --status

    Network devices using DPDK-compatible driver
    ============================================
    0000:00:07.0 '82545EM Gigabit Ethernet Controller (Copper)' drv=igb_uio unused=e1000
    0000:00:08.0 '82545EM Gigabit Ethernet Controller (Copper)' drv=igb_uio unused=e1000
    0000:00:09.0 '82545EM Gigabit Ethernet Controller (Copper)' drv=igb_uio unused=e1000

    Network devices using kernel driver
    ===================================
    0000:00:03.0 '82540EM Gigabit Ethernet Controller' if=eth0 drv=e1000 unused=igb_uio *Active*

    Other network devices
    =====================
    <none>

Now you can run tapbr like:

    $ sudo ./tapbr
    EAL: Detected 2 lcore(s)
    EAL: Probing VFIO support...
    EAL: WARNING: cpu flags constant_tsc=no nonstop_tsc=no -> using unreliable clock cycles !
    PMD: bnxt_rte_pmd_init() called for (null)
    EAL: PCI device 0000:00:03.0 on NUMA socket -1
    EAL:   probe driver: 8086:100e rte_em_pmd
    EAL: PCI device 0000:00:07.0 on NUMA socket -1
    EAL:   probe driver: 8086:100f rte_em_pmd
    EAL: PCI device 0000:00:08.0 on NUMA socket -1
    EAL:   probe driver: 8086:100f rte_em_pmd
    EAL: PCI device 0000:00:09.0 on NUMA socket -1
    EAL:   probe driver: 8086:100f rte_em_pmd
    Initializing port 0...
    Initialized network port 0 successfully.
    Initializing port 1...
    Initialized network port 1 successfully.
    Initializing port 2...
    Initialized network port 2 successfully.

    Bridge interfaces: 0 and 1
    Tap interface: 2

    PKTMBUF_POOL_SIZE=8191
    PKTMBUF_POOL_CACHE_SIZE=512
    RX_DESC_PER_QUEUE=1024
    TX_DESC_PER_QUEUE=1024
    BURST_SIZE=512
    OUTPUT_RING_SIZE=1024

    Processing queue 0 on CPU 1.

If you want the mirrored packet to be distributed between four DPDK
rings named myring0, myring1, myring2 and myring3, run:

    $ sudo ./tapbr -- -R myring -N 4

Notice that you can pass any number of DPDK-specific arguments before
the `--`. To see the complete list of DPDK command-line options use:

    $ ./tapbr -h

To get the complete list of `tapbr` options run:

    $ sudo ./tapbr -- -?

## DBus

`tapbr` attempts exposing a dbus interface over the system bus. The
system bus however, denies access to daemons not explicitly
allowed. In order to allow `tapbr` access to the system bus, copy
tapbr.conf to `/etc/dbus-1/system.d/`.

There is a `tapbrctl.py` script that can be used to query or control
`tapbr`. This script uses the dbus interface for its operation.
