
#include "cla/zephyr/cla_ml2cap.h"

#include "cla/mtcp_proto.h"
#include "cla/zephyr/cla_ml2cap.h"

#include "platform/hal_config.h"
#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_types.h"

#include "ud3tn/bundle_agent_interface.h"
#include "ud3tn/common.h"
#include "ud3tn/config.h"
#include "ud3tn/task_tags.h"
#include "ud3tn/node.h"

#include "routing/router_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/addr.h>
#include <net/buf.h>

#include "ud3tn/simplehtab.h"
#include "cla/zephyr/nb_ble.h"
#include "routing/router_task.h"

// TODO: Move this to KConfig
#define CONFIG_ML2CAP_PSM 0xc0
// we use a long queue to prevent deadlocks while tearing down connections TODO: we could also spawn a new thread for that
#define CONFIG_ML2CAP_CONN_INFO_QUEUE_SIZE (2*CONFIG_BT_MAX_CONN+2)

enum ml2cap_connection_info_event_type {
    ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_UP,
    ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_DOWN,
    ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_UP,
    ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_DOWN
};

struct ml2cap_connection_info_event {
    struct bt_conn *conn;
    enum ml2cap_connection_info_event_type type;
};

struct ml2cap_config {
    struct cla_config base;

    struct htab_entrylist *link_htab_elem[CONFIG_BT_MAX_CONN]; // we should not have really more entries than active connections?
    struct htab link_htab;
    Semaphore_t link_htab_sem;


    struct bt_l2cap_server l2cap_server;

    Task_t management_task;
    QueueIdentifier_t management_connection_info_queue;
};

// TODO: this is a singleton to support zephyr's callbacks without user data - should be refactored once possible
// it is initialized during the launch task
static struct ml2cap_config *s_ml2cap_config;

struct ml2cap_link {
    struct cla_link base;

    // The underlying bluetooth LE connection
    struct bt_conn *conn;

    struct bt_l2cap_le_chan chan;

    // Note that we use the MTCP data format!
    struct parser mtcp_parser;

    struct ml2cap_config *config;

    /* RX queue to handle zephyr's asynchronous recv callbacks */
    QueueIdentifier_t rx_queue;

    /* ml2cap://<Other ble device address in hex> */
    char *cla_addr;

    /* <Other ble device address in hex> */
    char *mac_addr;

    bool is_client;
    bool chan_connected;
};


static const char *ml2cap_name_get(void) {
    return "ml2cap";
}


/**
 * Signals that a connection is up to the router (so that bundles could be routed to this cla address)
 * The pointer to the cla_addr will be pushed to the router (which has to free it then)
 * TODO: This currently means that a link is closed
 */
static void signal_router_conn_up(const struct ml2cap_link *const ml2cap_link) {
    struct ml2cap_config *const ml2cap_config = ml2cap_link->config;

    struct router_signal rt_signal = {
            .type = ROUTER_SIGNAL_CONN_UP,
            .data = strdup(ml2cap_link->cla_addr),
    };

    const struct bundle_agent_interface *const bundle_agent_interface =
            ml2cap_config->base.bundle_agent_interface;
    hal_queue_push_to_back(bundle_agent_interface->router_signaling_queue,
                           &rt_signal);
}


/**
 * Signals that a connection is down to the router (so that no more bundles will be routed to this cla address)
 * The pointer to the cla_addr will be pushed to the router (which has to free it then)
 * TODO: This currently means that a link is closed
 */
static void signal_router_conn_down(const struct ml2cap_link *const ml2cap_link) {
    struct ml2cap_config *const ml2cap_config = ml2cap_link->config;

    struct router_signal rt_signal = {
            .type = ROUTER_SIGNAL_CONN_DOWN,
            .data = strdup(ml2cap_link->cla_addr),
    };

    const struct bundle_agent_interface *const bundle_agent_interface =
            ml2cap_config->base.bundle_agent_interface;
    hal_queue_push_to_back(bundle_agent_interface->router_signaling_queue,
                           &rt_signal);
}


/**
 * We pump discovered neighbors through the CLA layer as we add the correct CLA address.
 */
static void handle_discovered_neighbor_info(void *context, const struct nb_ble_node_info * const ble_node_info) {

    struct ml2cap_config *const ml2cap_config = (struct ml2cap_config *const) context;

    //LOGF("ML2CAP: Found other device with mac_address %s and eid %s", ble_node_info->mac_addr, ble_node_info->eid);

    // we now build the corresponding node entry and push it to the router which would then decide how to proceed with this contact
    // node will be freed by the router
    struct node* node = node_create(ble_node_info->eid);

    if(!node) {
        LOG("ML2CAP: Error while creating the node for NB");
        return;
    }

    node->cla_addr = cla_get_cla_addr(ml2cap_name_get(), ble_node_info->mac_addr);

    struct router_signal rt_signal = {
            .type = ROUTER_SIGNAL_NEIGHBOR_DISCOVERED,
            .data = node,
    };
    const struct bundle_agent_interface *const bundle_agent_interface =
            ml2cap_config->base.bundle_agent_interface;
    hal_queue_push_to_back(bundle_agent_interface->router_signaling_queue,
                           &rt_signal);



    bt_addr_le_t addr;
    bt_addr_le_from_mac_addr(ble_node_info->mac_addr, &addr);


    // TODO: It is a little bit excessive to directly connect to everyone in reach :)
    struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);

    if (conn == NULL) {
        // We now try to connect as soon as we received that advertisement
        nb_ble_stop(); // we need to disable the advertisements for that, Note that this cb is called by the NB_BLE TASK
        int err = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &conn);

        if (err) {
            if (err != -EINVAL) {
                // we get EINVAL in case the connection already exists...
                LOGF("ML2CAP: Failed to create connection (err %d)\n", err);
            }
        }
    }

    if (conn) {
        // conn was either set by bt_conn_lookup_addr_le or bt_conn_le_create
        // we directly unref the connection here
        bt_conn_unref(conn);
    }
}

/**
 * Imprtant: Need to take and release the semaphore before this operation!
 * @param ml2cap_config
 * @param cla_addr
 * @return */
static struct ml2cap_link *get_link_from_connection(struct ml2cap_config *ml2cap_config, struct bt_conn *conn) {

    const char *mac_addr = bt_addr_le_to_mac_addr(bt_conn_get_dst(conn));
    const char *cla_addr = cla_get_cla_addr(ml2cap_name_get(), mac_addr);

    struct ml2cap_link *link = htab_get(
            &ml2cap_config->link_htab,
            cla_addr
    );

    free((void*)mac_addr);
    free((void*)cla_addr);

    return link;
}

static void chan_connected_cb(struct bt_l2cap_chan *chan) {
    struct ml2cap_config *ml2cap_config = s_ml2cap_config;

    LOG("Channel connected!");
    struct ml2cap_connection_info_event ev = {
        .conn = bt_conn_ref(chan->conn),
        .type = ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_UP
    };
    hal_queue_push_to_back(ml2cap_config->management_connection_info_queue, &ev);
}

static void chan_disconnected_cb(struct bt_l2cap_chan *chan) {
    struct ml2cap_config *ml2cap_config = s_ml2cap_config;

    LOG("Channel disconnected!");

    struct ml2cap_connection_info_event ev = {
        .conn = bt_conn_ref(chan->conn),
        .type = ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_DOWN
    };

    hal_queue_push_to_back(ml2cap_config->management_connection_info_queue, &ev);
}

/**
 *
 */
static int chan_recv_cb(struct bt_l2cap_chan *chan, struct net_buf *buf) {
    struct ml2cap_config *ml2cap_config = s_ml2cap_config;

    hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);

    struct ml2cap_link *link = get_link_from_connection(ml2cap_config, chan->conn);

    if (link) {
        size_t num_elements = net_buf_frags_len(buf);
        for (int i = 0; i < num_elements; i++) {
            // TODO: This message queue abuse is quite inefficient!
            // TODO: Check that this conversion is correct!
            size_t b = (size_t) net_buf_pull_u8(buf);
            hal_queue_push_to_back(link->rx_queue, (void *) &b);
        }
    }
    // TODO: Can we release this already before?
    // TODO: We might want to introduce more specific semaphores?
    hal_semaphore_release(ml2cap_config->link_htab_sem);
    return 0;
}

static struct ml2cap_link *ml2cap_link_create(
    struct ml2cap_config *const ml2cap_config,
    struct bt_conn *conn
) {
    struct ml2cap_link *ml2cap_link =
            malloc(sizeof(struct ml2cap_link));

    if (!ml2cap_link) {
        LOG("ML2CAP: Failed to allocate memory!");
        return NULL;
    }

    ml2cap_link->conn = bt_conn_ref(conn);
    ml2cap_link->chan_connected = false; // will be initialized once available

    if (!ml2cap_link->conn) {
        LOG("ML2CAP: Failed to ref connection!");
        goto fail_after_alloc;
    }

    // we get the info if we are client or server
    struct bt_conn_info info;
    if (bt_conn_get_info(ml2cap_link->conn, &info)) {
        LOG("ML2CAP: Failed to get connection info!");
        goto fail_after_ref;
    }

    ml2cap_link->is_client = info.role == BT_CONN_ROLE_MASTER;

    ml2cap_link->config = ml2cap_config;

    static struct bt_l2cap_chan_ops chan_ops = {
            .connected = chan_connected_cb,
            .disconnected = chan_disconnected_cb,
            .recv = chan_recv_cb
    };

    ml2cap_link->chan.chan.ops = &chan_ops;

    ml2cap_link->mac_addr = bt_addr_le_to_mac_addr(bt_conn_get_dst(conn));
    if (!ml2cap_link->mac_addr) {
        LOG("ML2CAP: Failed to get mac_addr!");
        goto fail_after_ref;
        
    }

    ml2cap_link->cla_addr = cla_get_cla_addr(ml2cap_name_get(), ml2cap_link->mac_addr);
    
    if (!ml2cap_link->cla_addr) {
        LOG("ML2CAP: Failed to get cla_addr!");
        goto fail_after_mac_addr;
    }

    ml2cap_link->rx_queue = hal_queue_create(
            COMM_RX_QUEUE_LENGTH,
            sizeof(uint8_t)
    );

    if (!ml2cap_link->rx_queue) {
        LOG("ML2CAP: Failed to allocate rx queue!");
        goto fail_after_cla_addr;
    }
    
    mtcp_parser_reset(&ml2cap_link->mtcp_parser);

    return ml2cap_link;
    
    fail_after_rx_queue:
    hal_queue_delete(ml2cap_link->rx_queue);

    fail_after_cla_addr:
    free(ml2cap_link->cla_addr);

    fail_after_mac_addr:
    free(ml2cap_link->mac_addr);

    fail_after_ref:
    bt_conn_unref(ml2cap_link->conn);

    fail_after_alloc:
    free(ml2cap_link);
    return NULL;
}

static void ml2cap_link_destroy(struct ml2cap_link *ml2cap_link) {
    mtcp_parser_reset(&ml2cap_link->mtcp_parser);
    hal_queue_delete(ml2cap_link->rx_queue);
    free(ml2cap_link->cla_addr);
    free(ml2cap_link->mac_addr);
    bt_conn_unref(ml2cap_link->conn);
    free(ml2cap_link);
}

static void connected(struct bt_conn *conn, uint8_t err) {

    struct ml2cap_config *ml2cap_config = s_ml2cap_config;

    if (err) {
        LOGF("BLE Connection failed (err 0x%02x)\n", err);
    } else {
        LOG("BLE Connected\n");

        struct ml2cap_config *ml2cap_config = s_ml2cap_config; 

        struct ml2cap_connection_info_event ev = {
            .conn = bt_conn_ref(conn),
            .type = ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_UP
        };

        ;
        hal_queue_push_to_back(ml2cap_config->management_connection_info_queue, &ev);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    LOGF("BLE Disconnected (reason 0x%02x)\n", reason);

    struct ml2cap_config *ml2cap_config = s_ml2cap_config; 

    struct ml2cap_connection_info_event ev = {
        .conn = bt_conn_ref(conn),
        .type = ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_DOWN
    };

    hal_queue_push_to_back(ml2cap_config->management_connection_info_queue, &ev);
}

int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan) {

    struct ml2cap_config *ml2cap_config = s_ml2cap_config;

    hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);
    struct ml2cap_link *link = get_link_from_connection(s_ml2cap_config, conn);

    int ret = 0;
    if (link) {
        *chan = &link->chan.chan;
        // we mark the link as not connected, so we essentially
        // TODO: handle_channel_up? link->chan_connected = true;
        LOG("ML2CAP: l2cap_accept! TODO: Does this trigger the correct event?");
    } else {
        LOG("ML2CAP: l2cap_accept could not find related link entry!");
        ret = -EACCES;
    }
    hal_semaphore_release(ml2cap_config->link_htab_sem);
    return ret;
}



/**
 * This function handles a new connection. We try to connect directly if we are the client.
 * 
 * 
 * */
static void handle_connection_up(struct ml2cap_config *ml2cap_config, struct bt_conn *conn) {

    struct ml2cap_link *ml2cap_link = ml2cap_link_create(ml2cap_config, conn);

    if (ml2cap_link != NULL) {

        hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);
        struct htab_entrylist *htab_entry = htab_add(
                &ml2cap_config->link_htab,
                ml2cap_link->cla_addr,
                ml2cap_link
        );
        // we can release straight away as we only need to synchronize with other threads that read htab
        hal_semaphore_release(ml2cap_config->link_htab_sem);

        if (htab_entry != NULL) {
            if (ml2cap_link->is_client) {
                int err = bt_l2cap_chan_connect(ml2cap_link->conn, &ml2cap_link->chan.chan, CONFIG_ML2CAP_PSM);
                if (err) {
                    LOGF("ML2CAP: Could not try to connect to channel for \"%s\"", ml2cap_link->cla_addr);
                    // we disconnect, this will eventually also clear this link
                    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); 
                } else {
                     LOGF("ML2CAP: Added entry for \"%s\"", ml2cap_link->cla_addr);
                }
            }
        } else {
            // oops 
            LOGF("ML2CAP: htab entry already exists for \"%s\"!", ml2cap_link->cla_addr);
            ml2cap_link_destroy(ml2cap_link); // we need to destroy this link immediately (as it is not in the htab!)
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    } else {
        LOGF("ML2CAP: Could not create link entry for \"%s\"", ml2cap_link->cla_addr);
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void handle_connection_down(struct ml2cap_config *ml2cap_config, struct bt_conn *conn) {

    LOG("handle_connection_down START");


    hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);
    struct ml2cap_link *ml2cap_link = get_link_from_connection(ml2cap_config, conn);

    if (ml2cap_link != NULL) {
        // we can delete this as the tx and rx tasks do not need to lookup the link!
        htab_remove(
            &ml2cap_config->link_htab,
            ml2cap_link->cla_addr
        );
    }

    hal_semaphore_release(ml2cap_config->link_htab_sem);

    // now we clean everything up!

    if (ml2cap_link) {
        if (ml2cap_link->chan_connected) {
            // the connection is up and running -> rx and tx tasks have been initialized!
            // so we first first disable them!
            cla_generic_disconnect_handler(&ml2cap_link->base);

            ml2cap_link->chan_connected = false; // this will bring down the rx task
           
            // we also notify the router
            signal_router_conn_down(ml2cap_link);

            // and wait for clean-up
            // TODO: This could take a while and might block connection handling? (we remove, notify and release therefore before this call)
            

            LOG("handle_connection_down A");
            cla_link_wait_cleanup(&ml2cap_link->base);
            LOG("handle_connection_down B");
        }

         // only thing left -> free link ressources again
        ml2cap_link_destroy(ml2cap_link);
    }
    LOG("handle_connection_down END");
}

// TODO: We should also check what channel this is ;)
static void handle_channel_up(struct ml2cap_config *ml2cap_config, struct bt_conn *conn) {
    hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);
    struct ml2cap_link *ml2cap_link = get_link_from_connection(ml2cap_config, conn);
    hal_semaphore_release(ml2cap_config->link_htab_sem);

    // ml2cap_link->chan.chan is already initialized

    if (cla_link_init(&ml2cap_link->base, &ml2cap_config->base) == UD3TN_OK) {
        ml2cap_link->chan_connected = true; // this means that we are initialized and connected!
        signal_router_conn_up(ml2cap_link);
    } else {
        LOG("ML2CAP: Error initializing CLA link!");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void handle_channel_down(struct ml2cap_config *ml2cap_config, struct bt_conn *conn) {
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); // nothing else todo!
}

static void mtcp_management_task(void *param) {
    struct ml2cap_config *const ml2cap_config = (struct ml2cap_config *) param;

    // we setup the L2CAP server to handle incoming connections
    int err = bt_enable(NULL);
    if (err) {
        LOGF("Bluetooth init failed (err %d)", err);
        goto terminate;
    }

    static struct bt_conn_cb conn_callbacks = {
            .connected = connected,
            .disconnected = disconnected,
    };

    bt_conn_cb_register(&conn_callbacks);
    LOG("Bluetooth init done");


    LOG("Starting NB BLE");

    struct nb_ble_config config = {
            .eid = ml2cap_config->base.bundle_agent_interface->local_eid, // This gets copied
            .discover_cb = handle_discovered_neighbor_info,
            .discover_cb_context = ml2cap_config
    };

    if (nb_ble_launch(&config) != UD3TN_OK) {
        goto terminate;
    }

    ml2cap_config->l2cap_server.psm = CONFIG_ML2CAP_PSM;
    ml2cap_config->l2cap_server.accept = l2cap_accept;

    err = bt_l2cap_server_register(&ml2cap_config->l2cap_server);
    if (err) {
        LOG("ML2CAP: Error while registering L2CAP Server");
        goto terminate;
    }
    LOG("ML2CAP: Registered L2CAP Server");

    // we loop through the events
    while (true) {

        struct ml2cap_connection_info_event ev; 
        if (hal_queue_receive(ml2cap_config->management_connection_info_queue, &ev, -1) == UD3TN_OK) {
            
            if (ev.type == ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_UP) {
                LOG("handle event conn up");
                handle_connection_up(ml2cap_config, ev.conn);
            } else if (ev.type == ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_UP) {
                LOG("handle event chan up");
                handle_channel_up(ml2cap_config, ev.conn);
            } else if (ev.type == ML2CAP_CONNECTION_INFO_EVENT_TYPE_CHANNEL_DOWN) {
                LOG("handle event chan down");
                handle_channel_down(ml2cap_config, ev.conn);
            } else if (ev.type == ML2CAP_CONNECTION_INFO_EVENT_TYPE_CONNECTION_DOWN) {
                LOG("handle event conn down");
                handle_connection_down(ml2cap_config, ev.conn);
            } else {
                LOG("ML2CAP: Unknown event");
            }
            // we now only need to unref the connection
            bt_conn_unref(ev.conn);
        } else {
            LOG("ML2CAP: Could not receive queue entry...");
        }
    }

    terminate:
    hal_task_delete(ml2cap_config->management_task);
    free(param);
}

static enum ud3tn_result ml2cap_launch(struct cla_config *const config) {
    struct ml2cap_config *const ml2cap_config = (struct ml2cap_config *) config;

    ml2cap_config->management_task = hal_task_create(
            mtcp_management_task,
            "ml2cap_mgmt_t",
            CONTACT_LISTEN_TASK_PRIORITY, // TODO
            ml2cap_config,
            CONTACT_LISTEN_TASK_STACK_SIZE, // TODO
            (void *) CLA_SPECIFIC_TASK_TAG
    );

    if (!ml2cap_config->management_task)
        return UD3TN_FAIL;

    return UD3TN_OK;
}

size_t ml2cap_mbs_get(struct cla_config *const config) {
    (void) config;
    return SIZE_MAX;
}

void ml2cap_reset_parsers(struct cla_link *link) {
    struct ml2cap_link *const ml2cap_link = (struct ml2cap_link *) link;

    rx_task_reset_parsers(&link->rx_task_data);

    mtcp_parser_reset(&ml2cap_link->mtcp_parser);
    link->rx_task_data.cur_parser = &ml2cap_link->mtcp_parser;
}

size_t ml2cap_forward_to_specific_parser(struct cla_link *link,
                                         const uint8_t *buffer, size_t length) {
    struct ml2cap_link *const ml2cap_link = (struct ml2cap_link *) link;
    struct rx_task_data *const rx_data = &link->rx_task_data;
    size_t result = 0;

    // Decode MTCP CBOR byte string header if not done already
    if (!(ml2cap_link->mtcp_parser.flags & PARSER_FLAG_DATA_SUBPARSER))
        return mtcp_parser_parse(&ml2cap_link->mtcp_parser,
                                 buffer, length);

    // We do not allow to parse more than the stated length...
    if (length > ml2cap_link->mtcp_parser.next_bytes)
        length = ml2cap_link->mtcp_parser.next_bytes;

    switch (rx_data->payload_type) {
        case PAYLOAD_UNKNOWN:
            result = select_bundle_parser_version(rx_data, buffer, length);
            if (result == 0)
                ml2cap_reset_parsers(link);
            break;
        case PAYLOAD_BUNDLE6:
            rx_data->cur_parser = rx_data->bundle6_parser.basedata;
            result = bundle6_parser_read(
                    &rx_data->bundle6_parser,
                    buffer,
                    length
            );
            break;
        case PAYLOAD_BUNDLE7:
            rx_data->cur_parser = rx_data->bundle7_parser.basedata;
            result = bundle7_parser_read(
                    &rx_data->bundle7_parser,
                    buffer,
                    length
            );
            break;
        default:
            ml2cap_reset_parsers(link);
            return 0;
    }

    ASSERT(result <= ml2cap_link->mtcp_parser.next_bytes);
    ml2cap_link->mtcp_parser.next_bytes -= result;

    // All done
    if (!ml2cap_link->mtcp_parser.next_bytes)
        ml2cap_reset_parsers(link);

    return result;
}


static enum ud3tn_result ml2cap_read(struct cla_link *link,
                                     uint8_t *buffer, size_t length,
                                     size_t *bytes_read) {
    struct ml2cap_link *const ml2cap_link =
            (struct ml2cap_link *) link;


    // Special case: empty buffer
    if (length == 0) {
        *bytes_read = 0;
        return UD3TN_OK;
    }

    // Write-pointer to the current buffer position
    uint8_t *stream = buffer;

    QueueIdentifier_t rx_queue = ml2cap_link->rx_queue;

    // Receive at least one byte in blocking manner from the RX queue
    while (hal_queue_receive(rx_queue, stream, COMM_RX_TIMEOUT) != UD3TN_OK) {
        // it might be that we have disconnected while waiting...
        if (!ml2cap_link->chan_connected) {
            *bytes_read = 0;
            LOG("FAILED RX!");
            return UD3TN_FAIL;
        }
    }

    

    length--;
    stream++;

    // Emulate the behavior of recv() by reading further bytes with a very
    // small timeout.
    while (length--) {
        if (hal_queue_receive(rx_queue, stream,
                              COMM_RX_TIMEOUT) != UD3TN_OK)
            break;
        stream++;
    }

    *bytes_read = stream - buffer;


    return UD3TN_OK;
}

static enum ud3tn_result ml2cap_start_scheduled_contact(
        struct cla_config *config, const char *eid, const char *cla_addr) {
    // STUB - UNUSED
    (void) config;
    (void) eid;
    (void) cla_addr;

    // TODO: we need to use bt_addr_le_from_str to parse the cla_address again

    return UD3TN_OK;
}

static enum ud3tn_result ml2cap_end_scheduled_contact(
        struct cla_config *config, const char *eid, const char *cla_addr) {
    // STUB - UNUSED
    (void) config;
    (void) eid;
    (void) cla_addr;

    return UD3TN_OK;
}

// TODO: This net buf pool should be worked into ml2cap_send_packet_data when dynamic allocation is allowed
// This currently assumes that we only have one call to ml2cap_send_packet_data active at a time (see tx_task)
// TODO: We might want to use more than CONFIG_BT_MAX_CONN buffers, (who knows?)
K_SEM_DEFINE(ml2cap_send_packet_data_pool_sem, CONFIG_BT_MAX_CONN, CONFIG_BT_MAX_CONN);

// This destroy callback ensures that we do not allocate too many buffers
static void ml2cap_send_packet_data_pool_buf_destroy(struct net_buf *buf) {
    net_buf_destroy(buf);
    k_sem_give(&ml2cap_send_packet_data_pool_sem);
}

// TODO: we might need to define a fixed memory region and i.e. limit the maximum packet size
NET_BUF_POOL_FIXED_DEFINE(ml2cap_send_packet_data_pool, CONFIG_BT_MAX_CONN,
  CONFIG_BT_L2CAP_TX_MTU+BT_L2CAP_CHAN_SEND_RESERVE,
    ml2cap_send_packet_data_pool_buf_destroy
);


static void l2cap_transmit_bytes(struct cla_link *link, const void *data, const size_t length) {

    struct ml2cap_link *const ml2cap_link = (struct ml2cap_link *) link;
    struct ml2cap_config *const ml2cap_config = (struct ml2cap_config *) link->config;
    (void) ml2cap_config;


    // overall length could be more than the supported MTU -> we need to
    uint32_t mtu = ml2cap_link->chan.tx.mtu;

    uint32_t sent = 0;

    while (sent < length && ml2cap_link->chan_connected) {
        //LOGF("l2cap_transmit_bytes sent %d", sent);
        uint32_t frag_size = MIN(mtu, length - sent);

        // TODO: Not ideal to use the hal method here but define using zephyr specific macro!
        // This semaphore also ensures that we limit the maximum amout of stalled data (waiting to be sent by zephyr)
        hal_semaphore_take_blocking(&ml2cap_send_packet_data_pool_sem);

        // K_NO_WAIT is used per specification of NET_BUF_POOL_HEAP_DEFINE

        size_t buf_size = BT_L2CAP_CHAN_SEND_RESERVE+mtu;
        struct net_buf *buf = net_buf_alloc_len(&ml2cap_send_packet_data_pool, buf_size, K_NO_WAIT);


        // buf->len + sdu_hdr_len > ch->tx.mps?!
        if (!buf) {
            LOG("ml2cap: net_buf_alloc_len failed");
            k_sem_give(&ml2cap_send_packet_data_pool_sem);
            bt_conn_disconnect(ml2cap_link->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            link->config->vtable->cla_disconnect_handler(link);
            return;
        }

        net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
        net_buf_add_mem(buf, ((char *)data) + sent, frag_size);

        int ret = bt_l2cap_chan_send(&ml2cap_link->chan.chan, buf);

        if (ret < 0) {
            net_buf_unref(buf); // this should also release our semaphore
            LOG("ml2cap: ml2cap_send_packet_data failed");
            bt_conn_disconnect(ml2cap_link->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return;
        }

        // we sent the data successfully, it might got buffered for us, buffer will get released eventually
        sent += frag_size;
    }
}

static void ml2cap_begin_packet(struct cla_link *link, size_t length) {

    const size_t BUFFER_SIZE = 9; // max. for uint64_t
    uint8_t buffer[BUFFER_SIZE];

    const size_t hdr_len = mtcp_encode_header(buffer, BUFFER_SIZE, length);

    l2cap_transmit_bytes(link, buffer, hdr_len);
}

static void ml2cap_end_packet(struct cla_link *link) {
    // STUB
    (void) link;
}


void ml2cap_send_packet_data(struct cla_link *link, const void *data, const size_t length) {
    l2cap_transmit_bytes(link, data, length);
}

static struct cla_tx_queue ml2cap_get_tx_queue(
        struct cla_config *config, const char *eid, const char *cla_addr) {
    (void) eid;
    struct ml2cap_config *const ml2cap_config = (struct ml2cap_config *) config;

    //LOGF("ml2cap_get_tx_queue addr: %s", cla_addr);

    hal_semaphore_take_blocking(ml2cap_config->link_htab_sem);

    struct ml2cap_link *link = htab_get(
            &ml2cap_config->link_htab,
            cla_addr
    );

    if (link && link->chan_connected) {
        struct cla_link *const cla_link = &link->base;

        hal_semaphore_take_blocking(cla_link->tx_queue_sem);
        hal_semaphore_release(ml2cap_config->link_htab_sem);

        // Freed while trying to obtain it
        if (!cla_link->tx_queue_handle)
            return (struct cla_tx_queue) {NULL, NULL};

        return (struct cla_tx_queue) {
                .tx_queue_handle = cla_link->tx_queue_handle,
                .tx_queue_sem = cla_link->tx_queue_sem,
        };
    }

    hal_semaphore_release(ml2cap_config->link_htab_sem);
    return (struct cla_tx_queue) {NULL, NULL};
}

const struct cla_vtable ml2cap_vtable = {
        .cla_name_get = ml2cap_name_get,
        .cla_launch = ml2cap_launch,
        .cla_mbs_get = ml2cap_mbs_get,

        .cla_get_tx_queue = ml2cap_get_tx_queue,
        .cla_start_scheduled_contact = ml2cap_start_scheduled_contact,
        .cla_end_scheduled_contact = ml2cap_end_scheduled_contact,

        .cla_begin_packet = ml2cap_begin_packet,
        .cla_end_packet = ml2cap_end_packet,
        .cla_send_packet_data = ml2cap_send_packet_data,

        .cla_rx_task_reset_parsers = ml2cap_reset_parsers,
        .cla_rx_task_forward_to_specific_parser =
        ml2cap_forward_to_specific_parser,

        .cla_read = ml2cap_read,

        .cla_disconnect_handler = cla_generic_disconnect_handler,
};

static enum ud3tn_result ml2cap_init(
        struct ml2cap_config *config, const struct bundle_agent_interface *bundle_agent_interface) {
    /* Initialize base_config */
    if (cla_config_init(&config->base, bundle_agent_interface) != UD3TN_OK)
        return UD3TN_FAIL;

    config->management_connection_info_queue = hal_queue_create(CONFIG_ML2CAP_CONN_INFO_QUEUE_SIZE, sizeof(struct ml2cap_connection_info_event));

    if (!config->management_connection_info_queue) {
        return UD3TN_FAIL;
    }

    /* set base_config vtable */
    config->base.vtable = &ml2cap_vtable;

    // TODO: use other config variable
    htab_init(&config->link_htab, CONFIG_BT_MAX_CONN, config->link_htab_elem);
    config->link_htab_sem = hal_semaphore_init_binary();
    hal_semaphore_release(config->link_htab_sem);

    return UD3TN_OK;
}

struct cla_config *ml2cap_create(
        const char *const options[], const size_t option_count,
        const struct bundle_agent_interface *bundle_agent_interface) {

    (void) options;
    (void) option_count;

    if (s_ml2cap_config) {
        // TODO: Shall we handle restarts too?
        LOG("ml2cap: Already created");
        return NULL;
    }

    struct ml2cap_config *config = malloc(sizeof(struct ml2cap_config));

    if (!config) {
        LOG("ml2cap: Memory allocation failed!");
        return NULL;
    }

    if (ml2cap_init(config, bundle_agent_interface) != UD3TN_OK) {
        free(config);
        LOG("ml2cap: Initialization failed!");
        return NULL;
    }

    // TODO: Rework this singleton!
    s_ml2cap_config = config;

    LOG("ml2cap: Creation completed!");
    return &config->base;
}
