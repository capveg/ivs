/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

#include "ovs_driver_int.h"
#include "murmur/murmur.h"
#include <pthread.h>

#define IND_OVS_KFLOW_EXPIRATION_MS 2345
#define NUM_KFLOW_BUCKETS 8192

static struct list_head ind_ovs_kflows;
static struct list_head ind_ovs_kflow_buckets[NUM_KFLOW_BUCKETS];

static inline uint32_t
key_hash(const struct nlattr *key)
{
    return murmur_hash(nla_data(key), nla_len(key), ind_ovs_salt);
}

indigo_error_t
ind_ovs_kflow_add(struct ind_ovs_flow *flow,
                  const struct nlattr *key,
                  const struct nlattr *actions)
{
    /* Check input port accounting */
    struct nlattr *in_port_attr = nla_find(nla_data(key), nla_len(key), OVS_KEY_ATTR_IN_PORT);
    assert(in_port_attr);
    uint32_t in_port = nla_get_u32(in_port_attr);
    struct ind_ovs_port *port = ind_ovs_ports[in_port];
    if (port == NULL) {
        /* The port was deleted after the packet was queued to userspace. */
        return INDIGO_ERROR_NONE;
    }

    if (!ind_ovs_benchmark_mode && port->num_kflows >= IND_OVS_MAX_KFLOWS_PER_PORT) {
        LOG_WARN("port %d (%s) exceeded allowed number of kernel flows", in_port, port->ifname);
        return INDIGO_ERROR_RESOURCE;
    }

    uint32_t hash = key_hash(key);

    /*
     * Check that the kernel flow table doesn't already include this flow.
     * In the time between the packet being queued to userspace and the kflow
     * being inserted many more packets matching this kflow could have been
     * enqueued.
     */
    struct list_head *bucket = &ind_ovs_kflow_buckets[hash % NUM_KFLOW_BUCKETS];
    struct list_links *cur;
    LIST_FOREACH(bucket, cur) {
        struct ind_ovs_kflow *kflow2 = container_of(cur, bucket_links, struct ind_ovs_kflow);
        if (nla_len(kflow2->key) == nla_len(key) &&
            memcmp(nla_data(kflow2->key), nla_data(key), nla_len(key)) == 0) {
            return INDIGO_ERROR_NONE;
        }
    }

    int ovs_key_len = nla_attr_size(nla_len(key));
    struct ind_ovs_kflow *kflow = malloc(sizeof(*kflow) + ovs_key_len);
    if (kflow == NULL) {
        return INDIGO_ERROR_RESOURCE;
    }

    struct nl_msg *msg = ind_ovs_create_nlmsg(ovs_flow_family, OVS_FLOW_CMD_NEW);
    nla_put(msg, OVS_FLOW_ATTR_KEY, nla_len(key), nla_data(key));
    nla_put(msg, OVS_FLOW_ATTR_ACTIONS, nla_len(actions), nla_data(actions));
    if (ind_ovs_transact(msg) < 0) {
        free(kflow);
        return INDIGO_ERROR_UNKNOWN;
    }

    kflow->priority = flow->fte.priority;
    kflow->last_used = monotonic_us()/1000;
    kflow->flow = flow;
    kflow->in_port = in_port;
    kflow->stats.n_packets = 0;
    kflow->stats.n_bytes = 0;

    memcpy(kflow->key, key, ovs_key_len);

    list_push(&flow->kflows, &kflow->flow_links);
    list_push(&ind_ovs_kflows, &kflow->global_links);
    list_push(bucket, &kflow->bucket_links);

    port->num_kflows++;

    return INDIGO_ERROR_NONE;
}

void
ind_ovs_kflow_sync_stats(struct ind_ovs_kflow *kflow)
{
    struct nl_msg *msg = ind_ovs_create_nlmsg(ovs_flow_family, OVS_FLOW_CMD_GET);
    nla_put(msg, OVS_FLOW_ATTR_KEY, nla_len(kflow->key), nla_data(kflow->key));

    struct nlmsghdr *reply;
    if (ind_ovs_transact_reply(msg, &reply) < 0) {
        LOG_WARN("failed to sync flow stats");
        return;
    }

    struct nlattr *attrs[OVS_FLOW_ATTR_MAX+1];
    if (genlmsg_parse(reply, sizeof(struct ovs_header),
                      attrs, OVS_FLOW_ATTR_MAX,
                      NULL) < 0) {
        LOG_ERROR("failed to parse datapath message");
        abort();
    }

    struct nlattr *stats_attr = attrs[OVS_FLOW_ATTR_STATS];
    if (stats_attr) {
        struct ovs_flow_stats *stats = nla_data(stats_attr);
        kflow->stats = *stats;
    }

    struct nlattr *used_attr = attrs[OVS_FLOW_ATTR_USED];
    if (used_attr) {
        uint64_t used = nla_get_u64(used_attr);
        if (used > kflow->last_used) {
            kflow->last_used = used;
        } else {
            //LOG_WARN("kflow used time went backwards");
        }
    }

    free(reply);
}

/*
 * Delete the given kflow from the kernel flow table and free it.
 * This function should rarely be called directly. Instead use
 * ind_ovs_kflow_invalidate, which can attempt to update the kflow
 * with the correct actions. Deleting an active kflow could cause
 * a flood of upcalls, and inactive kflows will be expired anyway.
 */
static void
ind_ovs_kflow_delete(struct ind_ovs_kflow *kflow)
{
    struct ind_ovs_port *port = ind_ovs_ports[kflow->in_port];
    if (port) {
        port->num_kflows--;
    }

    /*
     * Packets could match the kernel flow in the time between syncing stats
     * and deleting it, but in practice we should not be deleting active flows.
     */
    ind_ovs_kflow_sync_stats(kflow);

    struct nl_msg *msg = ind_ovs_create_nlmsg(ovs_flow_family, OVS_FLOW_CMD_DEL);
    nla_put(msg, OVS_FLOW_ATTR_KEY, nla_len(kflow->key), nla_data(kflow->key));
    (void) ind_ovs_transact(msg);

    __sync_fetch_and_add(&kflow->flow->packets, kflow->stats.n_packets);
    __sync_fetch_and_add(&kflow->flow->bytes, kflow->stats.n_bytes);

    list_remove(&kflow->flow_links);
    list_remove(&kflow->global_links);
    list_remove(&kflow->bucket_links);
    free(kflow);
}

/*
 * Run the given kflow's key through the flowtable. If it matches a flow
 * then update the actions, otherwise delete it.
 */
void
ind_ovs_kflow_invalidate(struct ind_ovs_kflow *kflow)
{
    struct ind_ovs_parsed_key pkey;
    ind_ovs_parse_key(kflow->key, &pkey);

    /* Lookup the flow in the userspace flowtable. */
    struct ind_ovs_flow *flow;
    if (ind_ovs_lookup_flow(&pkey, &flow) != 0) {
        ind_ovs_kflow_delete(kflow);
        return;
    }

    struct nl_msg *msg = ind_ovs_create_nlmsg(ovs_flow_family, OVS_FLOW_CMD_SET);

    nla_put(msg, OVS_FLOW_ATTR_KEY, nla_len(kflow->key), nla_data(kflow->key));

    ind_ovs_translate_actions(&pkey, &flow->effects.apply_actions,
                              msg, OVS_FLOW_ATTR_ACTIONS);

    if (ind_ovs_transact(msg) < 0) {
        LOG_ERROR("Failed to modify kernel flow");
        return;
    }

    kflow->flow = flow;
    list_remove(&kflow->flow_links);
    list_push(&flow->kflows, &kflow->flow_links);
}

/* Check whether pkt_key is contained within flow_key/flow_mask */
static bool
cfr_match__(const struct ind_ovs_cfr *flow_key,
            const struct ind_ovs_cfr *flow_mask,
            const struct ind_ovs_cfr *pkt_key)
{
    uint64_t *f = (uint64_t *)flow_key;
    uint64_t *m = (uint64_t *)flow_mask;
    uint64_t *p = (uint64_t *)pkt_key;
    int i;

    for (i = 0; i < sizeof(*flow_key)/sizeof(*f); i++) {
        if ((p[i] & m[i]) != f[i]) {
            return false;
        }
    }

    return true;
}

/*
 * Invalidate all kflows that overlap the given match.
 */
void
ind_ovs_kflow_invalidate_overlap(const struct ind_ovs_cfr *flow_fields,
                                 const struct ind_ovs_cfr *flow_masks,
                                 uint16_t priority)
{
    struct list_links *cur, *next;
    LIST_FOREACH_SAFE(&ind_ovs_kflows, cur, next) {
        struct ind_ovs_kflow *kflow = container_of(cur, global_links, struct ind_ovs_kflow);
        if (kflow->priority >= priority) {
            continue;
        }

        struct ind_ovs_parsed_key pkey;
        ind_ovs_parse_key(kflow->key, &pkey);
        struct ind_ovs_cfr kflow_fields;
        ind_ovs_key_to_cfr(&pkey, &kflow_fields);

        if (cfr_match__(flow_fields, flow_masks, &kflow_fields)) {
            LOG_VERBOSE("invalidating kflow (overlap)");
            ind_ovs_kflow_invalidate(kflow);
        }
    }
}

/*
 * Invalidate all kflows that use a FLOOD or ALL action.
 * This is done when the set of ports changes.
 * TODO keep a list of these flows to optimize this.
 */
void
ind_ovs_kflow_invalidate_flood(void)
{
    struct list_links *cur, *next;
    LIST_FOREACH_SAFE(&ind_ovs_kflows, cur, next) {
        struct ind_ovs_kflow *kflow = container_of(cur, global_links, struct ind_ovs_kflow);
        if (kflow->flow->effects.flood) {
            LOG_VERBOSE("invalidating kflow (flood)");
            ind_ovs_kflow_invalidate(kflow);
        }
    }
}

/*
 * Delete all kflows that haven't been used in more than
 * IND_OVS_KFLOW_EXPIRATION_MS milliseconds.
 *
 * TODO do this more efficiently, spread out over multiple steps.
 */
void
ind_ovs_kflow_expire(void)
{
    uint64_t cur_time = monotonic_us()/1000;
    struct list_links *cur, *next;
    LIST_FOREACH_SAFE(&ind_ovs_kflows, cur, next) {
        struct ind_ovs_kflow *kflow = container_of(cur, global_links, struct ind_ovs_kflow);

        /* Don't bother checking kflows that can't have expired yet. */
        if ((cur_time - kflow->last_used) < IND_OVS_KFLOW_EXPIRATION_MS) {
            continue;
        }

        /* Might have expired, ask the kernel for the real last_used time. */
        ind_ovs_kflow_sync_stats(kflow);

        if ((cur_time - kflow->last_used) >= IND_OVS_KFLOW_EXPIRATION_MS) {
            LOG_VERBOSE("expiring kflow");
            ind_ovs_kflow_delete(kflow);
        }
    }
}

void
ind_ovs_kflow_module_init(void)
{
    list_init(&ind_ovs_kflows);

    int i;
    for (i = 0; i < NUM_KFLOW_BUCKETS; i++) {
        list_init(&ind_ovs_kflow_buckets[i]);
    }
}
