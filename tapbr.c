#include "dbus.h"

#include <rte_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_ring.h>

#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define TAPBR_VERSION_MAJOR 0
#define TAPBR_VERSION_MINOR 1
#define TAPBR_VERSION_REVISION 0

#define RING_PREFIX_MAX_SIZE 256

size_t total_pkts = 0;
size_t if0_pkts = 0;
size_t if1_pkts = 0;
size_t tx_drops = 0;
size_t ring_enq_drops = 0;
size_t tap_drops = 0;

static const struct argp_option options[] = {
  {"version", 'V', 0, 0, "Print program version and exit.", 0},
  {"queues", 'q', "NQUEUES", 0, "The number of queues used for receiving "
   "and transmitting on network interfaces.", 0},
  {"intf1", '1', "INTF1", 0, "First bridge interface. Defaults to 0.", 0},
  {"intf2", '2', "INTF2", 0, "Second bridge interface. Defaults to 1.", 0},
  {"tap", 'T', "TAP-INTF", 0, "Tap interface. Defaults to 2.", 0},
  {"ring-prefix", 'R', "RING-PREFIX", 0, "Ring name prefix. Instead of "
   "sending mirrored packets to a network interface, send them to a number "
   "of DPDK rings (determined by --rings option). The rings will names will "
   "be made up of this prefix and a sequential number. Defaults to 'tapbr'. "
   "In order to use DPDK rings as output, you need to use at least one of "
   "--rings and --ring-prefix options.", 0},
  {"rings", 'N', "NRINGS", 0, "The number of rings to use as output. Defaults "
   "to 1. In order to use DPDK rings as output, you need to use at least one "
   "of --rings and --ring-prefix options. Notice that load balancing between "
   "multiple rings is only supported when network interfaces support RSS.", 0},
  { 0 }
};

static char doc[] = "tapbr is a DPDK-based, mirroring bridge. It bridges "
  "two network interfaces while sending a copy of all of the traffic to "
  "either a third interface or a number of DPDK rings.";

static char args_doc[] = "[TAPBR OPTIONS]";

struct arguments {
  int queues;
  int intf1;
  int intf2;
  int tap;
  char ring_prefix[RING_PREFIX_MAX_SIZE];
  int rings;
};

volatile int keep_running = 1;

static int PKTMBUF_POOL_SIZE = ((1 << 13) - 1);
static int PKTMBUF_POOL_CACHE_SIZE = 512;
static int RX_DESC_PER_QUEUE = 1024;
static int TX_DESC_PER_QUEUE = 1024;
static int BURST_SIZE = 512;
static int OUTPUT_RING_SIZE = (1 << 10);

static uint8_t symmetric_hash_key[40] =
  {0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
   0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
   0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
   0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a};
#define symmetric_hash_key_len 40

static struct rte_eth_conf eth_conf = {
  .rxmode = {
    .max_rx_pkt_len = ETHER_MAX_LEN,
    .split_hdr_size = 0,
    .header_split = 0,
    .hw_ip_checksum = 0,
    .hw_vlan_filter = 0,
    .hw_vlan_strip = 0,
    .jumbo_frame = 0,
    .hw_strip_crc = 0,
    .mq_mode = ETH_MQ_RX_RSS,
  },
  .rx_adv_conf = {
    .rss_conf = {
      .rss_key = symmetric_hash_key,
      .rss_hf  = ETH_RSS_PROTO_MASK,
    },
  },
};

static struct rte_mempool *rx_pool;

static struct rte_ring **output_rings;

/* associates a queue number with each lcore */
static int lcore_queues[RTE_MAX_LCORE];

static void
signal_handler(int signum)
{
  (void) signum;
  keep_running = 0;
}

static int
bridge_routine(void *arg)
{
  int npkts, i, j, q, ret, id, intf, other_intf, ring_idx;
  struct rte_mbuf **pkts;
  struct rte_mbuf **clones;
  struct arguments *args = arg;

  pkts = (struct rte_mbuf **) malloc(sizeof(struct rte_mbuf *) * BURST_SIZE);
  clones = (struct rte_mbuf **) malloc(sizeof(struct rte_mbuf *) * BURST_SIZE);

  /* Get the index of the queue associated with this lcore. */
  id = rte_lcore_id();
  q = lcore_queues[id];
  if (q < 0)
    return 1;

  fprintf(stderr, "Processing queue %d on CPU %d.\n", q, id);

  while (likely(keep_running)) {
    for (i = 0; i < 2; ++i) {
      intf = (i == 0) ? args->intf1 : args->intf2;
      other_intf = (i == 1) ? args->intf1 : args->intf2;
      npkts = rte_eth_rx_burst(intf, q, pkts, BURST_SIZE);
      if (npkts == 0) {
        continue;
      }

      fprintf(stderr, "Got %d packets.\n", npkts);
      total_pkts += npkts;
      if (i == 0)
        if0_pkts += npkts;
      else
        if1_pkts += npkts;

      for (j = 0; j < npkts; ++j) {
        clones[j] = rte_pktmbuf_clone(pkts[j], rx_pool);
      }

      ret = rte_eth_tx_burst(other_intf, q, pkts, npkts);
      if (ret != npkts) {
        fprintf(stderr,
                "Could not write all %d packets into TX ring %d of port %d. "
                "%d packets dropped.\n",
                npkts, q, i, npkts - ret);
        for (j = ret; j < npkts; j++) {
          rte_pktmbuf_free(pkts[j]);
        }
        tx_drops += npkts - ret;
      }

      if (*args->ring_prefix) {
        /* send mirrored packets to output rings */
        for (j = 0; j < npkts; ++j) {
          ring_idx = pkts[j]->hash.rss % args->rings;
          ret = rte_ring_enqueue(output_rings[ring_idx], pkts[j]);
          if (ret == -EDQUOT) {
            fprintf(stderr, "Quota exceeded in output ring: %s%d.\n",
                    args->ring_prefix, ring_idx);
          } else if (ret == -ENOBUFS) {
            fprintf(stderr, "Not enough room in output ring: %s%d.\n",
                    args->ring_prefix, ring_idx);
            rte_pktmbuf_free(pkts[j]);
            ++ring_enq_drops;
          }
        }
      } else {
        /* send mirrored packets to a tap interface */
        ret = rte_eth_tx_burst(args->tap, q, clones, npkts);
        if (ret != npkts) {
          printf("Could not write all %d packets into TX ring %d of port %d. "
                 "%d packets dropped.\n",
                 npkts, q, args->tap, npkts - ret);
          for (j = ret; j < npkts; j++) {
            rte_pktmbuf_free(pkts[j]);
          }
          tap_drops += npkts - ret;
        }
      }
    }
  }

  free(pkts);
  free(clones);

  return 0;
}

static int
get_next_lcore_id(int last)
{
  int i;

  i = rte_get_next_lcore(last, 0, 0);
  if (i != RTE_MAX_LCORE)
    return i;

  rte_exit(EXIT_FAILURE, "Not enough CPU cores.\n");
}

static void
read_int_environment_var(const char *name, int *target)
{
  char *endptr;
  char *env;
  int value;

  env = getenv(name);
  if (env && *env) {
    value = strtol(env, &endptr, 10);
    if (*endptr) {
      printf("Invalid value for %s environment variable.\n", name);
      exit(1);
    }

    *target = value;
  }
}

static int
is_power_of_two(unsigned int x)
{
  return ((x != 0) && !(x & (x - 1)));
}

static void
read_environ(void)
{
  read_int_environment_var("PKTMBUF_POOL_SIZE", &PKTMBUF_POOL_SIZE);
  if (!is_power_of_two(PKTMBUF_POOL_SIZE + 1)) {
    printf("PKTMUBF_POOL_SIZE must be a power of two minus one.\n");
    exit(1);
  }

  read_int_environment_var("PKTMBUF_POOL_CACHE_SIZE", &PKTMBUF_POOL_CACHE_SIZE);
  if (!is_power_of_two(PKTMBUF_POOL_CACHE_SIZE)) {
    printf("PKTMUBF_POOL_CACHE_SIZE must be a power of two.\n");
    exit(1);
  }

  read_int_environment_var("RX_DESC_PER_QUEUE", &RX_DESC_PER_QUEUE);
  if (!is_power_of_two(RX_DESC_PER_QUEUE)) {
    printf("RX_DESC_PER_QUEUE must be a power of two.\n");
    exit(1);
  }

  read_int_environment_var("TX_DESC_PER_QUEUE", &TX_DESC_PER_QUEUE);
  if (!is_power_of_two(TX_DESC_PER_QUEUE)) {
    printf("TX_DESC_PER_QUEUE must be a power of two.\n");
    exit(1);
  }

  read_int_environment_var("BURST_SIZE", &BURST_SIZE);
  if (!is_power_of_two(BURST_SIZE)) {
    printf("BURST_SIZE must be a power of two.\n");
    exit(1);
  }

  read_int_environment_var("OUTPUT_RING_SIZE", &OUTPUT_RING_SIZE);
  if (!is_power_of_two(OUTPUT_RING_SIZE)) {
    printf("OUTPUT_RING_SIZE must be a power of two.\n");
    exit(1);
  }
}

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
  struct arguments *args = state->input;
  char *end;

  switch (key) {
  case 'V':
    printf("tapbr v%d.%d.%d\n",
           TAPBR_VERSION_MAJOR,
           TAPBR_VERSION_MINOR,
           TAPBR_VERSION_REVISION);
    exit(EXIT_SUCCESS);

  case 'q':
    args->queues = strtol(arg, &end, 10);
    if (*arg == 0 || end == 0 || *end != 0)
      argp_error(state, "Invalid number passed to --queues.");
    break;

  case '1':
    args->intf1 = strtol(arg, &end, 10);
    if (*arg == 0 || end == 0 || *end != 0)
      argp_error(state, "Invalid number passed to --intf1.");
    break;

  case '2':
    args->intf2 = strtol(arg, &end, 10);
    if (*arg == 0 || end == 0 || *end != 0)
      argp_error(state, "Invalid number passed to --intf2.");
    break;

  case 'T':
    args->tap = strtol(arg, &end, 10);
    if (*arg == 0 || end == 0 || *end != 0)
      argp_error(state, "Invalid number passed to --tap.");
    break;

  case 'R':
    strncpy(args->ring_prefix, arg, RING_PREFIX_MAX_SIZE - 1);
    break;

  case 'N':
    args->rings = strtol(arg, &end, 10);
    if (*arg == 0 || end == 0 || *end != 0)
      argp_error(state, "Invalid number passed to --rings.");
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }

  return 0;
}

static void
read_args(int argc, char *argv[], struct arguments *args)
{
  struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

  /* set defaults */
  args->queues = 1;
  args->intf1 = 0;
  args->intf2 = 1;
  args->tap = -1;
  *args->ring_prefix = 0;
  args->rings = 0;

  argp_parse(&argp, argc, argv, 0, 0, args);

  if (args->tap != -1 && (args->rings || *args->ring_prefix)) {
    rte_exit(EXIT_FAILURE,
             "--tap cannot be used with --rings or --ring-prefix.\n");
  } else if (args->rings && !*args->ring_prefix) {
    strcpy(args->ring_prefix, "tapbr");
  } else if ((!args->rings) && *args->ring_prefix) {
    args->rings = 1;
  } else if (args->tap == -1 && !args->rings && !*args->ring_prefix) {
    args->tap = 2;
  }
}

int
main(int argc, char *argv[])
{
  struct arguments args;
  int ret, i, j, id, intf;
  char ring_name[RING_PREFIX_MAX_SIZE + 20];

  read_environ();

  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE,
             "Could not initialize EAL: %s\n", rte_strerror(rte_errno));
  }

  argc -= ret;
  argv += ret;

  read_args(argc, argv, &args);

  /* create mempool */
  rx_pool = rte_pktmbuf_pool_create(
    "rx_pool",
    PKTMBUF_POOL_SIZE,
    PKTMBUF_POOL_CACHE_SIZE,
    0,
    RTE_MBUF_DEFAULT_BUF_SIZE,
    rte_socket_id());

  /* configure interfaces */
  if (!*args.ring_prefix && rte_eth_dev_count() < 3) {
    rte_exit(EXIT_FAILURE,
             "The tap bridge needs three interfaces to function.\n");
  }

  if (args.intf1 >= rte_eth_dev_count()) {
    rte_exit(EXIT_FAILURE, "No such interface: %d\n", args.intf1);
  }

  if (args.intf2 >= rte_eth_dev_count()) {
    rte_exit(EXIT_FAILURE, "No such interface: %d\n", args.intf2);
  }

  if (!*args.ring_prefix && args.tap >= rte_eth_dev_count()) {
    rte_exit(EXIT_FAILURE, "No such interface: %d\n", args.tap);
  }

  /* create output rings if necessary */
  if (*args.ring_prefix) {
    fprintf(stderr, "Creating output rings...\n");
    output_rings = malloc(sizeof(struct rte_ring *) * args.rings);
    for (i = 0; i < args.rings; i++) {
      snprintf(ring_name, sizeof(ring_name), "%s%d", args.ring_prefix, i);
      output_rings[i] = rte_ring_create(ring_name,
                                        OUTPUT_RING_SIZE,
                                        SOCKET_ID_ANY,
                                        RING_F_SP_ENQ);
    }
  }

  /* associate an lcore with each queue */

  /* first set all cores to queue -1 and context to NULL. */
  for (i = 0; i < RTE_MAX_LCORE; i++) {
    lcore_queues[i] = -1;
  }

  for (i = 0; i < args.queues; i++) {
    id = get_next_lcore_id(0);
    lcore_queues[id] = i;
  }

  for (i = 0; i < 3; ++i) {
    switch (i) {
    case 0:
      intf = args.intf1;
      break;

    case 1:
      intf = args.intf2;
      break;

    case 2:
      intf = args.tap;
      if (intf == -1)
        continue;
      break;
    }

    fprintf(stderr, "Initializing port %d...\n", intf);
    if (rte_eth_dev_configure(intf, args.queues, args.queues, &eth_conf) < 0) {
      rte_exit(EXIT_FAILURE, "Could not configure network port %d.\n", intf);
    }

    for (j = 0; j < args.queues; ++j) {
      ret = rte_eth_tx_queue_setup(intf, j,
                                   TX_DESC_PER_QUEUE,
                                   rte_eth_dev_socket_id(intf),
                                   0);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE,
                 "Could not setup TX queue %d on network port %d.\n",
                 j, intf);
      }
    }

    for (j = 0; j < args.queues; j++) {
      /* TODO: Use multiple pktmbuf pools one for each socket and send
         the appropriate one as the last argument. */
      ret = rte_eth_rx_queue_setup(intf, j,
                                   RX_DESC_PER_QUEUE,
                                   rte_eth_dev_socket_id(intf),
                                   0,
                                   rx_pool);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE,
                 "Could not setup RX queue %d on network port %d.\n",
                 j, intf);
      }
    }

    if (rte_eth_dev_start(intf) < 0) {
      rte_exit(EXIT_FAILURE, "Could not start network port %d.\n", intf);
    }

    rte_eth_promiscuous_enable(intf);

    fprintf(stderr, "Initialized network port %d successfully.\n", intf);
  }

  /* display bridge info */
  fprintf(stderr, "\n");
  fprintf(stderr, "Bridge interfaces: %d and %d\n", args.intf1, args.intf2);
  if (*args.ring_prefix) {
    fprintf(stderr, "Tap rings: ");
    for (i = 0; i < args.rings; i++) {
      fprintf(stderr, "%s%d", args.ring_prefix, i);
      if (i != args.rings - 1)
        fprintf(stderr, ", ");
    }
    fprintf(stderr, "\n");
  } else {
    fprintf(stderr, "Tap interface: %d\n", args.tap);
  }

  fprintf(stderr, "\n");
  fprintf(stderr, "PKTMBUF_POOL_SIZE=%d\n", PKTMBUF_POOL_SIZE);
  fprintf(stderr, "PKTMBUF_POOL_CACHE_SIZE=%d\n", PKTMBUF_POOL_CACHE_SIZE);
  fprintf(stderr, "RX_DESC_PER_QUEUE=%d\n", RX_DESC_PER_QUEUE);
  fprintf(stderr, "TX_DESC_PER_QUEUE=%d\n", TX_DESC_PER_QUEUE);
  fprintf(stderr, "BURST_SIZE=%d\n", BURST_SIZE);
  fprintf(stderr, "OUTPUT_RING_SIZE=%d\n", OUTPUT_RING_SIZE);
  fprintf(stderr, "\n");

  /* setup signal handlers */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* launch threads */
  ret = dbus_launch();
  if (ret)
    fprintf(stderr,
            "Could not start dbus interface. Make sure tapbr is allowed access "
            "to system bus by copying tapbr.conf to /etc/dbus-1/system.d/\n");

  rte_eal_mp_remote_launch(bridge_routine, &args, CALL_MASTER);

  /* wait on the threads */
  RTE_LCORE_FOREACH_SLAVE(i) {
    if (rte_eal_wait_lcore(i) < 0) {
      rte_exit(EXIT_FAILURE,
               "Slave thread returned with a non-zero error code.\n");
    }
  }

  dbus_finalize();

  free(output_rings);
  fprintf(stderr, "\n");

  return 0;
}
