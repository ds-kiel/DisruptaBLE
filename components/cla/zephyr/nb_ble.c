#include "cla/zephyr/nb_ble.h"


#include "ud3tn/common.h"
#include "ud3tn/config.h"
#include "ud3tn/task_tags.h"

#include "platform/hal_config.h"
#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"
#include "platform/hal_types.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <net/buf.h>
#include <stdlib.h>

// TODO: Refine this size...
#define NB_BLE_QUEUE_SIZE 10
#define CONFIG_ML2CAP_SERVICE_UUID BT_UUID_GAP_VAL

// TODO: Not the best to define them statically...
static struct nb_ble_config nb_ble_config;
static QueueIdentifier_t nb_ble_node_info_queue;

static const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(CONFIG_ML2CAP_SERVICE_UUID))
};

static void device_found_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                            struct net_buf_simple *ad) {
    if (nb_ble_node_info_queue == NULL) {
        LOG("NB BLE: Queue not found!");
        return;
    }

    char dev[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, dev, sizeof(dev));
    LOGF("[NB BLE]: %s, AD evt type %u, AD data len %u, RSSI %i\n",
           dev, type, ad->len, rssi);

    /* We're only interested in connectable events */
    if (type == BT_GAP_ADV_TYPE_ADV_IND ||
        type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {

        struct nb_ble_node_info node_info = {
            .eid = strdup("test"),
            .mac_addr = strdup(dev)
        };

        // we simply queue this to prevent BLE thread blocking
        if(hal_queue_try_push_to_back(nb_ble_node_info_queue, &node_info, 0) != UD3TN_OK) {
            LOG("NB BLE: Could not queue node info!");
        }
    }
}



static void nb_ble_management_task(void *param) {
    (void)param;

    nb_ble_node_info_queue = hal_queue_create(NB_BLE_QUEUE_SIZE, sizeof(struct nb_ble_node_info));

    if (!nb_ble_node_info_queue) {
        LOG("NB BLE: Failed to initialize queue");
        return;
    }

    // automatically start advertisements!
    nb_ble_start();
    while(true) {
        struct nb_ble_node_info node_info;
        if (hal_queue_receive(nb_ble_node_info_queue, &node_info, -1) == UD3TN_OK) {
            nb_ble_config.discover_cb(nb_ble_config.discover_cb_context, &node_info);
            free(node_info.eid);
            free(node_info.mac_addr);
        }
    }
    // free(nb_ble_config.eid)
}


void nb_ble_start() {
    int err = 0;
    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);

    if (err) {
        LOGF("NB BLE: advertising failed to start (err %d)\n", err);
    }

    /* Use active scanning and disable duplicate filtering to handle any
    * devices that might update their advertising data at runtime. */
    struct bt_le_scan_param scan_param = {
            .type       = BT_LE_SCAN_TYPE_PASSIVE,
            .options    = BT_LE_SCAN_OPT_NONE,
            .interval   = BT_GAP_SCAN_FAST_INTERVAL,    // TODO: Adapt interval and window
            .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_param, device_found_cb);

    if (err) {
        LOGF("NB BLE: Scanning failed to start (err %d)\n", err);
    }

}


void nb_ble_stop() {

    int err = bt_le_adv_stop();

    if (err) {
        LOGF("NB BLE: advertising failed to stop (err %d)\n", err);
    }

    err = bt_le_scan_stop();
    if (err) {
        LOGF("NB BLE: Scanning failed to stop (err %d)\n", err);
    }
}


/*
 * Launches a new task to handle BLE advertisements
 */
enum ud3tn_result nb_ble_launch(const struct nb_ble_config * const config) {

    if(config->discover_cb == NULL) {
        return UD3TN_FAIL;
    }

    if(config->eid == NULL) {
        return UD3TN_FAIL;
    }

    memcpy(&nb_ble_config, config, sizeof(struct nb_ble_config));

    nb_ble_config.eid = strdup(config->eid);

    Task_t task = hal_task_create(
            nb_ble_management_task,
            "nb_ble_mgmt_t",
            CONTACT_LISTEN_TASK_PRIORITY, // TODO
            NULL,
            CONTACT_LISTEN_TASK_STACK_SIZE, // TODO
            (void *) CLA_SPECIFIC_TASK_TAG
    );

    if (!task) {
        free(nb_ble_config.eid);
        return UD3TN_FAIL;
    }
    return UD3TN_OK;
}