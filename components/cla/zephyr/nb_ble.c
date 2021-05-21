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


// TODO: Make this configurable?
#define NB_BLE_UUID 0xFFFF

#ifndef CONFIG_NB_BLE_QUEUE_SIZE
#define CONFIG_NB_BLE_QUEUE_SIZE 10
#endif

#ifndef CONFIG_NB_BLE_SCAN_DELAY_MS
#define CONFIG_NB_BLE_SCAN_DELAY_MS 50
#endif

#ifndef CONFIG_NB_BLE_DEBUG
#define CONFIG_NB_BLE_DEBUG 0
#endif

// TODO: Not the best to define them statically...
static struct nb_ble_config nb_ble_config;
static QueueIdentifier_t nb_ble_node_info_queue;


static void device_found_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                            struct net_buf_simple *ad) {
    if (nb_ble_node_info_queue == NULL) {
        LOG("NB BLE: Queue not found!");
        return;
    }
    char dev[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, dev, sizeof(dev));
    //LOGF("[NB BLE]: %s, AD evt type %u, AD data len %u, RSSI %i\n", dev, type, ad->len, rssi);

    /* We're only interested in connectable events */
    if (type == BT_GAP_ADV_TYPE_ADV_IND ||
        type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {

        // TODO: This is not standard-conform advertisement parsing!
        if (ad->len >= 5) {
            /*uint8_t _flags_len = */ net_buf_simple_pull_u8(ad);
            /*uint8_t _flags_type = */ net_buf_simple_pull_u8(ad);
            /*uint8_t _flags = */ net_buf_simple_pull_u8(ad);
            uint8_t entry_len = net_buf_simple_pull_u8(ad);
            uint8_t type = net_buf_simple_pull_u8(ad);

            if (type == BT_DATA_SVC_DATA16 && entry_len >= 4) {

                uint8_t uuid_low = net_buf_simple_pull_u8(ad);
                uint8_t uuid_high = net_buf_simple_pull_u8(ad);

                if (((uuid_high<<8)|uuid_low) == NB_BLE_UUID) {

                    size_t eid_len = entry_len-3;
                    void *eid_buf = net_buf_simple_pull_mem(ad, eid_len);

                    // This will be freed later!
                    char *eid = malloc(eid_len+1);
                    memcpy(eid, eid_buf, eid_len);
                    eid[eid_len] = '\0';

                    struct nb_ble_node_info node_info = {
                            .eid = eid,
                            .mac_addr = bt_addr_le_to_mac_addr(addr)
                    };

                    // we simply queue this to prevent BLE thread blocking
                    // This will copy the content from node_info into the queue
                    if(hal_queue_try_push_to_back(nb_ble_node_info_queue, &node_info, 0) != UD3TN_OK) {
                        LOG("NB BLE: Could not queue node info!");
                        free((void*)node_info.eid);
                        free((void*)node_info.mac_addr);
                    }
                }
            }
        }
    }
}



static void nb_ble_management_task(void *param) {
    (void)param;

    nb_ble_node_info_queue = hal_queue_create(CONFIG_NB_BLE_QUEUE_SIZE, sizeof(struct nb_ble_node_info));

    if (!nb_ble_node_info_queue) {
        LOG("NB BLE: Failed to initialize queue");
        return;
    }

    // automatically start advertisements!
    nb_ble_start();
    while(true) {
        struct nb_ble_node_info node_info;
        if (hal_queue_receive(nb_ble_node_info_queue, &node_info, CONFIG_NB_BLE_SCAN_DELAY_MS) == UD3TN_OK) {
            nb_ble_config.discover_cb(nb_ble_config.discover_cb_context, &node_info);
            free((void*)node_info.eid);
            free((void*)node_info.mac_addr);
        } else {
            // TODO: This is not optimal!
            nb_ble_start(); // we always try to start the scanning afterward
        }
    }
    // free(nb_ble_config.eid)
}


void nb_ble_start() {
    int err = 0;

    size_t data_len = strlen(nb_ble_config.eid)+2; // 2 byte uuid
    char *data = malloc(data_len);

    // Store the UUID in little endian format
    *data = NB_BLE_UUID&0xFF;
    *(data+1) = (NB_BLE_UUID>>8)&0xFF;
    memcpy(data+2, nb_ble_config.eid, data_len-2);  // this copies the data without the null terminator

    struct bt_data ad[] = {
            BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
            BT_DATA(BT_DATA_SVC_DATA16, data, data_len)
    };

    err = bt_le_adv_start(
            BT_LE_ADV_CONN,
            ad,
            ARRAY_SIZE(ad), NULL, 0);

    free(data);

#if CONFIG_NB_BLE_DEBUG
    if (err) {
        LOGF("NB BLE: advertising failed to start (err %d)\n", err);
    }
#endif

    /* Use active scanning and disable duplicate filtering to handle any
    * devices that might update their advertising data at runtime. */
    struct bt_le_scan_param scan_param = {
            .type       = BT_LE_SCAN_TYPE_PASSIVE,
            .options    = BT_LE_SCAN_OPT_NONE,
            .interval   = BT_GAP_SCAN_FAST_INTERVAL,    // TODO: Adapt interval and window
            .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_param, device_found_cb);

#if CONFIG_NB_BLE_DEBUG
    if (err) {
        LOGF("NB BLE: Scanning failed to start (err %d)\n", err);
    }
#endif

}


void nb_ble_stop() {

    int err = bt_le_adv_stop();

#if CONFIG_NB_BLE_DEBUG
    if (err) {
        LOGF("NB BLE: advertising failed to stop (err %d)\n", err);
    }
#endif

    err = bt_le_scan_stop();
#if CONFIG_NB_BLE_DEBUG
    if (err) {
        LOGF("NB BLE: Scanning failed to stop (err %d)\n", err);
    }
#endif
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
        free((void*)nb_ble_config.eid);
        return UD3TN_FAIL;
    }
    return UD3TN_OK;
}

/**
 * Format is "ED-71-8F-C2-E4-6E.random" while the zephyr format is "ED:71:8F:C2:E4:6E (random)"
 * Returns new string which needs to be freed afterward
 */
char* bt_addr_le_to_mac_addr(const bt_addr_le_t *addr) {

    ASSERT(addr);
    char type[10];

    switch (addr->type) {
        case BT_ADDR_LE_PUBLIC:
            strcpy(type, "public");
            break;
        case BT_ADDR_LE_RANDOM:
            strcpy(type, "random");
            break;
        case BT_ADDR_LE_PUBLIC_ID:
            strcpy(type, "public-id");
            break;
        case BT_ADDR_LE_RANDOM_ID:
            strcpy(type, "random-id");
            break;
        default:
            snprintk(type, sizeof(type), "0x%02x", addr->type);
            break;
    }

    char buf[BT_ADDR_LE_STR_LEN+1];
    snprintk(buf, BT_ADDR_LE_STR_LEN, "%02X-%02X-%02X-%02X-%02X-%02X.%s",
                    addr->a.val[5], addr->a.val[4], addr->a.val[3],
                    addr->a.val[2], addr->a.val[1], addr->a.val[0], type);

    buf[BT_ADDR_LE_STR_LEN] = '\0';
    return strdup(buf);
}

/**
 * We split "ED-71-8F-C2-E4-6E.random" -> into "ED-71-8F-C2-E4-6E" and "random"
 * @return Zero on success or (negative) error code otherwise.
 */
int bt_addr_le_from_mac_addr(const char *str, bt_addr_le_t *addr) {

    ASSERT(addr);
    ASSERT(str);
    ASSERT(strlen(str) > 18);

    char buf[BT_ADDR_LE_STR_LEN+1];
    strncpy(buf, str, BT_ADDR_LE_STR_LEN);
    buf[BT_ADDR_LE_STR_LEN] = '\0';

    // split the string
    buf[17] = '\0';
    char *mac = &buf[0];
    char *type = &buf[18];

    for(size_t i = 0; i < 17; i++) {
        char a = buf[i];
        if (a == '-') {
            buf[i] = ':';
        }
    }

    return bt_addr_le_from_str(mac, type, addr);
}