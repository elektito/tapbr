#include <rte_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>

#include <stdio.h>

#define NQUEUES 1

static int PKTMBUF_POOL_SIZE = ((1 << 13) - 1);
static int PKTMBUF_POOL_CACHE_SIZE = 512;
static int RX_DESC_PER_QUEUE = 1024;
static int TX_DESC_PER_QUEUE = 1024;
static int BURST_SIZE = 512;

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

struct rte_mempool *rx_pool;

static int
bridge_routine(void *arg)
{
  int npkts, i, j, q, ret;
  struct rte_mbuf *pkts[BURST_SIZE];
  struct rte_mbuf *clones[BURST_SIZE];

  (void) arg;

  printf("BRIDGE ROUTINE!\n");

  q = 0;
  fprintf(stderr, "Processing queue %d on CPU %d.\n", q, id);

  for (;;) {
    for (i = 0; i < 2; ++i) {
      npkts = rte_eth_rx_burst(i, q, pkts, BURST_SIZE);
      if (npkts == 0) {
        continue;
      }

      fprintf(stderr, "Got %d packets.\n", npkts);

      for (j = 0; j < npkts; ++j) {
        clones[j] = rte_pktmbuf_clone(pkts[j], rx_pool);
      }

      ret = rte_eth_tx_burst(i, q, pkts, npkts);
      if (ret != npkts) {
        fprintf(stderr,
                "Could not write all %d packets into TX ring %d of port %d. "
                "%d packets dropped.\n",
                npkts, q, i, npkts - ret);
        for (j = ret; j < npkts; j++) {
          rte_pktmbuf_free(pkts[j]);
        }
      }

      ret = rte_eth_tx_burst(2, q, clones, npkts);
      if (ret != npkts) {
        printf("Could not write all %d packets into TX ring %d of port %d. "
               "%d packets dropped.\n",
               npkts, q, i, npkts - ret);
        for (j = ret; j < npkts; j++) {
          rte_pktmbuf_free(pkts[j]);
        }
      }
    }
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  int ret, i, j;

  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE,
             "Could not initialize EAL: %s\n", rte_strerror(rte_errno));
  }

  /* create mempool */
  rx_pool = rte_pktmbuf_pool_create(
    "rx_pool",
    PKTMBUF_POOL_SIZE,
    PKTMBUF_POOL_CACHE_SIZE,
    0,
    RTE_MBUF_DEFAULT_BUF_SIZE,
    rte_socket_id());

  /* configure interfaces */
  if (rte_eth_dev_count() < 3) {
    rte_exit(EXIT_FAILURE,
             "The tap bridge needs three interfaces to function.\n");
  }

  for (i = 0; i < 3; ++i) {
    fprintf(stderr, "Initializing port %d...\n", i);
    if (rte_eth_dev_configure(i, NQUEUES, NQUEUES, &eth_conf) < 0) {
      rte_exit(EXIT_FAILURE, "Could not configure network port %d.\n", i);
    }

    for (j = 0; j < NQUEUES; ++j) {
      ret = rte_eth_tx_queue_setup(i, j,
                                   TX_DESC_PER_QUEUE,
                                   rte_eth_dev_socket_id(i),
                                   0);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE,
                 "Could not setup TX queue %d on network port %d.\n",
                 j, i);
      }
    }

    for (j = 0; j < NQUEUES; j++) {
      /* TODO: Use multiple pktmbuf pools one for each socket and send
         the appropriate one as the last argument. */
      ret = rte_eth_rx_queue_setup(i, j,
                                   RX_DESC_PER_QUEUE,
                                   rte_eth_dev_socket_id(i),
                                   0,
                                   rx_pool);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE,
                 "Could not setup RX queue %d on network port %d.\n",
                 j, i);
      }
    }

    if (rte_eth_dev_start(i) < 0) {
      rte_exit(EXIT_FAILURE, "Could not start network port %d.\n", i);
    }

    rte_eth_promiscuous_enable(i);

    fprintf(stderr, "Initialized network port %d successfully.\n", i);
  }

  /* launch threads */
  rte_eal_mp_remote_launch(bridge_routine, 0, CALL_MASTER);

  /* wait on the threads */
  RTE_LCORE_FOREACH_SLAVE(i) {
    if (rte_eal_wait_lcore(i) < 0) {
      rte_exit(EXIT_FAILURE,
               "Slave thread returned with a non-zero error code.\n");
    }
  }

  return 0;
}
