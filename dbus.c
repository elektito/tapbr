#include "dbus.h"

#include <systemd/sd-bus.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#define _cleanup_(x) __attribute__((cleanup(x)))

static pthread_t dbus_thread;
static sd_bus *bus = NULL;
static sd_bus_slot *slot = NULL;
static int started = 0;

extern int keep_running;

extern _Atomic size_t total_pkts;
extern _Atomic size_t if0_pkts;
extern _Atomic size_t if1_pkts;
extern _Atomic size_t tx_drops;
extern _Atomic size_t ring_enq_drops;
extern _Atomic size_t tap_drops;

static int
method_get_stats(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
  _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
  int r;

  (void) userdata;

  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0)
    return sd_bus_error_set_errno(ret_error, r);

  r = sd_bus_message_open_container(reply, 'a', "{st}");
  if (r < 0)
    return sd_bus_error_set_errno(ret_error, r);

#define APPEND_STAT(NAME, VALUE)                            \
  r = sd_bus_message_append(reply, "{st}", NAME, VALUE);    \
  if (r < 0)                                                \
    return sd_bus_error_set_errno(ret_error, r);

  APPEND_STAT("total_pkts", total_pkts);
  APPEND_STAT("if0_pkts", if0_pkts);
  APPEND_STAT("if1_pkts", if1_pkts);
  APPEND_STAT("tx_drops", tx_drops);
  APPEND_STAT("ring_enq_drops", ring_enq_drops);
  APPEND_STAT("tap_drops", tap_drops);
#undef APPEND_STAT

  r = sd_bus_message_close_container(reply);
  if (r < 0)
    return sd_bus_error_set_errno(ret_error, r);

  return sd_bus_send(NULL, reply, NULL);
}

static void *
dbus_routine(void *arg)
{
  int r;

  (void) arg;

  while (keep_running) {
    /* Process requests */
    r = sd_bus_process(bus, NULL);
    if (r < 0) {
      fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
      break;
    }

    /* We processed a request, try to process another one, right-away */
    if (r > 0)
      continue;

    /* Wait for the next request to process */
    r = sd_bus_wait(bus, (uint64_t) 100);
    if (r < 0) {
      fprintf(stderr, "Failed to wait on bus: %s\n", strerror(-r));
      break;
    }
  }

  return (void *)(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}

static const sd_bus_vtable tapbr_vtable[] = {
  SD_BUS_VTABLE_START(0),
  SD_BUS_METHOD("GetStats", "", "a{st}", method_get_stats, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_VTABLE_END
};

int
dbus_launch(void)
{
  int r;

  /* Connect to the user bus this time */
  r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
    goto exit_failure;
  }

  /* Install the object */
  r = sd_bus_add_object_vtable(bus,
                               &slot,
                               "/com/elektito/tapbr",  /* object path */
                               "com.elektito.tapbr",   /* interface name */
                               tapbr_vtable,
                               NULL);
  if (r < 0) {
    fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
    goto exit_failure;
  }

  /* Take a well-known service name so that clients can find us */
  r = sd_bus_request_name(bus, "com.elektito.tapbr", 0);
  if (r < 0) {
    fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-r));
    goto exit_failure;
  }

  /* Launch the server thread */
  pthread_create(&dbus_thread, 0, dbus_routine, 0);

  started = 1;

  return 0;

exit_failure:
  sd_bus_slot_unref(slot);
  sd_bus_unref(bus);

  return 1;
}

void
dbus_finalize(void)
{
  if (!started)
    return;

  pthread_join(dbus_thread, 0);

  sd_bus_slot_unref(slot);
  sd_bus_unref(bus);
}
