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
