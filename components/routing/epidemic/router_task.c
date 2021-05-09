#include "ud3tn/bundle.h"
#include "ud3tn/bundle_fragmenter.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/bundle_storage_manager.h"
#include "ud3tn/common.h"
#include "ud3tn/config.h"
#include "ud3tn/contact_manager.h"
#include "ud3tn/node.h"
#include "routing/contact/router.h"
#include "routing/contact/router_optimizer.h"
#include "routing/router_task.h"
#include "routing/contact/routing_table.h"
#include "ud3tn/task_tags.h"

#include "platform/hal_io.h"
#include "platform/hal_queue.h"
#include "platform/hal_semaphore.h"
#include "platform/hal_task.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Limit the amount of transmissions to nodes that are NOT the destination
// -1 Transmissions will not limit the amount of transmissions to other nodes
// Bundles will still be delivered to the destination node in all cases
#define EPIDEMIC_ROUTING_NUM_REPLICAS -1


// Limit the amount of hops to forward bundles to their destination
// 0 -> only directly deliver bundles to their destination (e.g. as in direct delivery)
// 1 -> only forward bundles once and then directly deliver them (e.g. as in spray-and-wait)
// -1 -> do not limit forwarding (e.g. classical epidemic flooding)
#define EPIDEMIC_ROUTING_FORWARDING -1


// While EPIDEMIC_ROUTING_NUM_REPLICAS and EPIDEMIC_ROUTING_FORWARDING need to be set to -1 for classical epidemic forwarding
// We can further tweak the settings to support both spray-and-wait and direct delivery (which are essentially limited variants)
// bundles are stored until their timeout


// TODO: Recheck https://tools.ietf.org/html/draft-irtf-dtnrg-ipnd-03 -> there the EID is part of the neighbor discovery
// Seems like we will also need to define neighbor discovery for BLE (!) -> we can split that to the BLE part and the initial connection setup? (e.g. BLE only GATT service UUID plus Service Data EID (hash?))
// THEN during connection setup use GATT / two channels ? / the one channel for setup by sending the same IPDTNNB packet on that reliable connection? -> covers ALL what we want... As we then know services, EID and cla address -> we can thus report a full node to the router! (not only the link address)
// the BLE NB could cache the nodes which are available at a specific point in time.



// TODO: Based on the link address, each party sends a hello message from their local endpoints to the routing AGENT
// This way, each can register the


// TODO: I think that we need to keep track of outdated bundles ourselves?
// TODO: Wrapping bundles in an Epidemic Routing Bundle could be nice?


// TODO: First send all bundles without checking which are alredy available? -> which will obviously be bad, then move on to send the distinct bundle hashes?!
// TODO: Use bundle source id, creation timestamp and sequence number together with the fragmentation (offset ?)


// TODO: I guess that we need to save the bundles (routed_bundles?!) that we want to (delete, forward?), deletion does not seem to be handled right now (-> select random bundle and check its timeout?
// TODO: Use hopcount! Seems fitting? -> but we need to make sure that we still deliver to the destination! Currently, hop count is only checked after local delivery! (but before routing so perfect for us?)


// TODO: We probably want to use the available contact struct to save the available links? Or we simply use custom helper structures for each connection to keep track of the bundles?


// TODO: Loop through all available contacts and pump connections full with bundles to send? (first send all bundles ofc)
// TODO: Do we also want to randomize this? (ignore duplicates?) or shall we use the timeouts as a way to determine forwarding?
// TODO: Maybe simply manage a htab of the routed_bundles locally too?

// TODO: We might want to limit the number of active connections?


static bool process_signal(
        struct router_signal signal,
        QueueIdentifier_t bp_signaling_queue,
        QueueIdentifier_t router_signaling_queue);

struct bundle_processing_result {
    int8_t status_or_fragments;
    bundleid_t fragment_ids[ROUTER_MAX_FRAGMENTS];
};

#define BUNDLE_RESULT_NO_ROUTE 0
#define BUNDLE_RESULT_NO_TIMELY_CONTACTS -1
#define BUNDLE_RESULT_NO_MEMORY -2
#define BUNDLE_RESULT_INVALID -3
#define BUNDLE_RESULT_EXPIRED -4

static struct bundle_processing_result process_bundle(struct bundle *bundle);

void router_task(void *rt_parameters)
{
    struct router_task_parameters *parameters;
    struct router_signal signal;

    ASSERT(rt_parameters != NULL);
    parameters = (struct router_task_parameters *)rt_parameters;

    for (;;) {
        if (hal_queue_receive(
                parameters->router_signaling_queue, &signal,
                -1) == UD3TN_OK
                ) {
            process_signal(signal,
                           parameters->bundle_processor_signaling_queue,
                           parameters->router_signaling_queue
                           );
        }
    }
}

static inline enum bundle_processor_signal_type get_bp_signal(int8_t bh_result)
{
    switch (bh_result) {
        case BUNDLE_RESULT_EXPIRED:
            return BP_SIGNAL_BUNDLE_EXPIRED;
        default:
            return BP_SIGNAL_FORWARDING_CONTRAINDICATED;
    }
}

static inline enum bundle_status_report_reason get_reason(int8_t bh_result)
{
    switch (bh_result) {
        case BUNDLE_RESULT_NO_ROUTE:
            return BUNDLE_SR_REASON_NO_KNOWN_ROUTE;
        case BUNDLE_RESULT_NO_MEMORY:
            return BUNDLE_SR_REASON_DEPLETED_STORAGE;
        case BUNDLE_RESULT_EXPIRED:
            return BUNDLE_SR_REASON_LIFETIME_EXPIRED;
        case BUNDLE_RESULT_NO_TIMELY_CONTACTS:
        default:
            return BUNDLE_SR_REASON_NO_TIMELY_CONTACT;
    }
}

static bool process_signal(
        struct router_signal signal,
        QueueIdentifier_t bp_signaling_queue,
        QueueIdentifier_t router_signaling_queue)
{
    bool success = true;
    bundleid_t b_id;
    struct bundle *b;
    struct routed_bundle *rb;
    struct contact *contact;
    struct router_command *command;
    struct node *node;

    switch (signal.type) {
        case ROUTER_SIGNAL_PROCESS_COMMAND:
            command = (struct router_command *)signal.data;
            LOGF("RouterTask: Command (T = %c) ignored.", command->type);
            free_node(command->data);
            free(command);
            break;
        case ROUTER_SIGNAL_ROUTE_BUNDLE:

            // TODO: Check if this bundle was actually meant for this router
            b_id = (bundleid_t)(uintptr_t)signal.data;
            b = bundle_storage_get(b_id);

            /*
             * TODO: Check bundle expiration time
             * => no timely contact signal
             */

            struct bundle_processing_result proc_result = {
                    .status_or_fragments = BUNDLE_RESULT_INVALID
            };

            if (b != NULL)
                proc_result = process_bundle(b);
            b = NULL; /* b may be invalid or free'd now */

            if (IS_DEBUG_BUILD)
                LOGF(
                        "RouterTask: Bundle #%d [ %s ] [ frag = %d ]",
                        b_id,
                        (proc_result.status_or_fragments < 1)
                        ? "ERR" : "OK",
                        proc_result.status_or_fragments
                );
            if (proc_result.status_or_fragments < 1) {

                const enum bundle_status_report_reason reason =
                        get_reason(proc_result.status_or_fragments);
                const enum bundle_processor_signal_type signal =
                        get_bp_signal(proc_result.status_or_fragments);

                bundle_processor_inform(
                        bp_signaling_queue,
                        b_id,
                        signal,
                        reason
                );
                success = false;
            } else {
                for (int8_t i = 0; i < proc_result.status_or_fragments;
                     i++) {
                    bundle_processor_inform(
                            bp_signaling_queue,
                            proc_result.fragment_ids[i],
                            BP_SIGNAL_BUNDLE_ROUTED,
                            BUNDLE_SR_REASON_NO_INFO
                    );
                }
            }
            break;
        case ROUTER_SIGNAL_CONTACT_OVER:
            LOG("ROUTER_SIGNAL_CONTACT_OVER not supported");
            break;
        case ROUTER_SIGNAL_TRANSMISSION_SUCCESS:
        case ROUTER_SIGNAL_TRANSMISSION_FAILURE:
            rb = (struct routed_bundle *)signal.data;
            if (rb->serialized == rb->contact_count) {
                b_id = rb->id;
                bundle_processor_inform(
                        bp_signaling_queue, b_id,
                        (rb->serialized == rb->transmitted)
                        ? BP_SIGNAL_TRANSMISSION_SUCCESS
                        : BP_SIGNAL_TRANSMISSION_FAILURE,
                        BUNDLE_SR_REASON_NO_INFO
                );
                free(rb->destination);
                free(rb->contacts);
                free(rb);
            }
            break;
        case ROUTER_SIGNAL_OPTIMIZATION_DROP:
            LOG("ROUTER_SIGNAL_OPTIMIZATION_DROP not supported");
            break;
        case ROUTER_SIGNAL_WITHDRAW_NODE:
            LOG("ROUTER_SIGNAL_WITHDRAW_NODE not supported");
            break;
        case ROUTER_SIGNAL_NEW_LINK_ESTABLISHED:
            LOG("ROUTER_SIGNAL_NEW_LINK_ESTABLISHED not supported");
            break;
        case ROUTER_SIGNAL_NEIGHBOR_DISCOVERED:
            ;//LOG("ROUTER_SIGNAL_NEIGHBOR_DISCOVERED");
            struct node *neighbor = (struct node *)signal.data;
            if(neighbor) {
                LOGF("RouterTask: Neighbor Discovered %s, %s", neighbor->eid, neighbor->cla_addr);
                free_node(neighbor);
            }
            break;
        default:
            LOGF("RouterTask: Invalid signal (%d) received!", signal.type);
            success = false;
            break;
    }
    return success;
}

static struct bundle_processing_result apply_fragmentation(
        struct bundle *bundle, struct router_result route);

static struct bundle_processing_result process_bundle(struct bundle *bundle)
{
    struct router_result route;
    struct bundle_processing_result result = {
            .status_or_fragments = BUNDLE_RESULT_NO_ROUTE
    };

    ASSERT(bundle != NULL);

    if (bundle_get_expiration_time_s(bundle) < hal_time_get_timestamp_s()) {
        // Bundle is already expired on arrival at the router...
        result.status_or_fragments = BUNDLE_RESULT_EXPIRED;
        return result;
    }

    // TODO: Support fragmentation, for now we do not fragment at all (which will hinder propagation of big bundles
    // If we however support fragmentation, the bundles will also need to be reconstructed before we can use them in this router -> implement router agent later
    // they could initially send small unfragmented "hello bundles" to exchange basic contact information before
    bool fragmentation_required = false;

    result.status_or_fragments = BUNDLE_RESULT_NO_ROUTE;
    /*if (fragmentation_required) {
        if (!bundle_must_not_fragment(bundle)) {
            // Only fragment if it is allowed -- if not, there is no route.
            result = apply_fragmentation(bundle, route);
        } else {
            result.status_or_fragments = BUNDLE_RESULT_NO_ROUTE;
        }
    }*/


    /* route = router_get_first_route(bundle);
    if (route.fragments == 1) {
    } else if (!bundle_must_not_fragment(bundle)) {
        // Only fragment if it is allowed -- if not, there is no route.
        result = apply_fragmentation(bundle, route);
    }*/

    return result;
}

static struct bundle_processing_result apply_fragmentation(
        struct bundle *bundle, struct router_result route)
{
    struct bundle *frags[ROUTER_MAX_FRAGMENTS];
    uint32_t size;
    int8_t f, g;
    uint8_t fragments = route.fragments;
    struct bundle_processing_result result = {
            .status_or_fragments = BUNDLE_RESULT_NO_MEMORY
    };

    /* Create fragments */
    frags[0] = bundlefragmenter_initialize_first_fragment(bundle);
    if (frags[0] == NULL)
        return result;

    for (f = 0; f < fragments - 1; f++) {
        /* Determine minimal fragmented bundle size */
        if (f == 0)
            size = bundle_get_first_fragment_min_size(bundle);
        else if (f == fragments - 1)
            size = bundle_get_last_fragment_min_size(bundle);
        else
            size = bundle_get_mid_fragment_min_size(bundle);

        frags[f + 1] = bundlefragmenter_fragment_bundle(frags[f],
                                                        size + route.fragment_results[f].payload_size);

        if (frags[f + 1] == NULL) {
            for (g = 0; g <= f; g++)
                bundle_free(frags[g]);
            return result;
        }
    }

    /* Add to route */
    for (f = 0; f < fragments; f++) {
        bundle_storage_add(frags[f]);
        if (!router_add_bundle_to_route(
                &route.fragment_results[f], frags[f])
                ) {
            for (g = 0; g < f; g++)
                router_remove_bundle_from_route(
                        &route.fragment_results[g],
                        frags[g]->id, 1);
            for (g = 0; g < fragments; g++) {
                /* FIXME: Routed bundles not unrouted */
                if (g <= f)
                    bundle_storage_delete(frags[g]->id);
                bundle_free(frags[g]);
            }
            return result;
        }
    }

    /* Success - remove bundle */
    bundle_storage_delete(bundle->id);
    bundle_free(bundle);

    for (f = 0; f < fragments; f++)
        result.fragment_ids[f] = frags[f]->id;
    result.status_or_fragments = fragments;
    return result;
}
