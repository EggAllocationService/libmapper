#include <lo/lo.h>
#include <stdlib.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#ifdef _MSC_VER
#include <windows.h>
#include <malloc.h>
#include "time.h"
#else 
#include <unistd.h>
#include <sys/time.h>
#endif
#include <assert.h>

#include <stddef.h>

#include "bitflags.h"
#include "device.h"
#include "graph.h"
#include "expression.h"
#include "link.h"
#include "list.h"
#include "map.h"
#include "network.h"
#include "mpr_signal.h"
#include "mpr_time.h"
#include "mpr_type.h"
#include "path.h"
#include "slot.h"
#include "table.h"
#include "thread_data.h"
#include "value.h"

#include "util/mpr_debug.h"

#include "config.h"
#include <mapper/mapper.h>

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
static void* device_thread_func(void *data);
#endif

#ifdef HAVE_WIN32_THREADS
static unsigned __stdcall device_thread_func(void *data);
#endif

extern const char* net_msg_strings[NUM_MSG_STRINGS];

#define MPR_DEV_STRUCT_ITEMS                                            \
    mpr_obj_t obj;      /* always first */                              \
    mpr_dev *linked;                                                    \
    char *name;         /*!< The full name for this device, or zero. */ \
    mpr_time synced;    /*!< Timestamp of last sync. */                 \
    int prefix_len;     /*!< Length of the prefix string. */            \
    int ordinal;                                                        \
    int num_inputs;     /*!< Number of associated input signals. */     \
    int num_outputs;    /*!< Number of associated output signals. */    \
    int num_maps_in;    /*!< Number of associated incoming maps. */     \
    int num_maps_out;   /*!< Number of associated outgoing maps. */     \
    int num_linked;     /*!< Number of linked devices. */               \
    int status;                                                         \
    uint8_t subscribed;

/*! A record that keeps information about a device. */
struct _mpr_dev {
    MPR_DEV_STRUCT_ITEMS
} mpr_dev_t;

typedef struct _mpr_subscriber {
    struct _mpr_subscriber *next;
    lo_address addr;
    uint32_t lease_exp;
    int flags;
} *mpr_subscriber;

/*! Allocated resources */
typedef struct _mpr_allocated_t {
    double count_time;          /*!< The last time collision count was updated. */
    double hints[8];            /*!< Availability of a range of resource values. */
    unsigned int val;           /*!< The resource to be allocated. */
    int collision_count;        /*!< The number of collisions detected. */
    uint8_t locked;             /*!< Whether or not the value has been locked (allocated). */
    uint8_t online;             /*!< Whether or not we are connected to the
                                 *   distributed allocation network. */
} mpr_allocated_t, *mpr_allocated;

struct _mpr_local_dev {
    MPR_DEV_STRUCT_ITEMS

    lo_server servers[4];

    mpr_allocated_t ordinal_allocator;  /*!< A unique ordinal for this device instance. */
    int registered;                     /*!< Non-zero if this device has been registered. */

    int n_output_callbacks;

    mpr_subscriber subscribers;         /*!< Linked-list of subscribed peers. */

    struct {
        struct _mpr_id_map **active;    /*!< The list of active instance id maps. */
        struct _mpr_id_map *reserve;    /*!< The list of reserve instance id maps. */
    } idmaps;

    mpr_expr_stack expr_stack;
    mpr_thread_data thread_data;

    mpr_time time;
    int num_sig_groups;
    uint8_t time_is_stale;
    uint8_t polling;
    uint8_t bundle_idx;
    uint8_t sending;
    uint8_t receiving;
} mpr_local_dev_t;

/* prototypes */
static void mpr_dev_start_servers(mpr_local_dev dev);
static void mpr_dev_remove_idmap(mpr_local_dev dev, int group, mpr_id_map rem);
MPR_INLINE static int _process_outgoing_maps(mpr_local_dev dev);

mpr_time ts = {0,1};

size_t mpr_dev_get_struct_size()
{
    return sizeof(mpr_dev_t);
}

static int _cmp_qry_linked(const void *ctx, mpr_dev dev)
{
    int i;
    mpr_dev self = *(mpr_dev*)ctx;
    for (i = 0; i < self->num_linked; i++) {
        if (!self->linked[i] || self->linked[i]->obj.id == dev->obj.id)
            return 1;
    }
    return 0;
}

static int _cmp_qry_sigs(const void *context_data, mpr_sig sig)
{
    mpr_id dev_id = *(mpr_id*)context_data;
    int dir = *(int*)((char*)context_data + sizeof(mpr_id));
    return ((dir & sig->dir) && (dev_id == sig->dev->obj.id));
}

void mpr_dev_init(mpr_dev dev, int is_local, const char *name, mpr_id id)
{
    int mod = is_local ? NON_MODIFIABLE : MODIFIABLE;
    mpr_tbl tbl;
    mpr_list qry;

    mpr_obj_set_is_local(&dev->obj, is_local);
    if (name) {
        assert(!dev->name);
        dev->name = strdup(name);
    }
    if (id) {
        assert(!dev->obj.id);
        dev->obj.id = id;
    }

    dev->obj.props.synced = mpr_tbl_new();
    if (!dev->obj.is_local)
        dev->obj.props.staged = mpr_tbl_new();
    tbl = dev->obj.props.synced;

    /* these properties need to be added in alphabetical order */
    mpr_tbl_link(tbl, PROP(DATA), 1, MPR_PTR, &dev->obj.data,
                 LOCAL_MODIFY | INDIRECT | LOCAL_ACCESS_ONLY);
    mpr_tbl_link(tbl, PROP(ID), 1, MPR_INT64, &dev->obj.id, mod);
    qry = mpr_graph_new_query(dev->obj.graph, 0, MPR_DEV, (void*)_cmp_qry_linked, "v", &dev);
    mpr_tbl_link(tbl, PROP(LINKED), 1, MPR_LIST, qry, NON_MODIFIABLE | PROP_OWNED);
    mpr_tbl_link(tbl, PROP(NAME), 1, MPR_STR, &dev->name, mod | INDIRECT | LOCAL_ACCESS_ONLY);
    mpr_tbl_link(tbl, PROP(NUM_MAPS_IN), 1, MPR_INT32, &dev->num_maps_in, mod);
    mpr_tbl_link(tbl, PROP(NUM_MAPS_OUT), 1, MPR_INT32, &dev->num_maps_out, mod);
    mpr_tbl_link(tbl, PROP(NUM_SIGS_IN), 1, MPR_INT32, &dev->num_inputs, mod);
    mpr_tbl_link(tbl, PROP(NUM_SIGS_OUT), 1, MPR_INT32, &dev->num_outputs, mod);
    mpr_tbl_link(tbl, PROP(ORDINAL), 1, MPR_INT32, &dev->ordinal, mod);
    if (!dev->obj.is_local) {
        qry = mpr_graph_new_query(dev->obj.graph, 0, MPR_SIG, (void*)_cmp_qry_sigs,
                                  "hi", dev->obj.id, MPR_DIR_ANY);
        mpr_tbl_link(tbl, PROP(SIG), 1, MPR_LIST, qry, NON_MODIFIABLE | PROP_OWNED);
    }
    mpr_tbl_link(tbl, PROP(STATUS), 1, MPR_INT32, &dev->status, mod | LOCAL_ACCESS_ONLY);
    mpr_tbl_link(tbl, PROP(SYNCED), 1, MPR_TIME, &dev->synced, mod | LOCAL_ACCESS_ONLY);
    mpr_tbl_link(tbl, PROP(VERSION), 1, MPR_INT32, &dev->obj.version, mod);

    if (dev->obj.is_local)
        mpr_tbl_set(tbl, PROP(LIBVER), NULL, 1, MPR_STR, PACKAGE_VERSION, NON_MODIFIABLE);
    mpr_tbl_set(tbl, PROP(IS_LOCAL), NULL, 1, MPR_BOOL, &dev->obj.is_local,
                LOCAL_ACCESS_ONLY | NON_MODIFIABLE);
}

/*! Allocate and initialize a device. This function is called to create a new
 *  mpr_dev, not to create a representation of remote devices. */
mpr_dev mpr_dev_new(const char *name_prefix, mpr_graph g)
{
    mpr_local_dev dev;
    RETURN_ARG_UNLESS(name_prefix, 0);
    if (name_prefix[0] == '/')
        ++name_prefix;
    TRACE_RETURN_UNLESS(!strchr(name_prefix, '/'), NULL, "error: character '/' "
                        "is not permitted in device name.\n");
    if (!g) {
        g = mpr_graph_new(0);
        mpr_graph_set_owned(g, 0);
    }

    dev = (mpr_local_dev)mpr_graph_add_list_item(g, MPR_DEV, sizeof(mpr_local_dev_t));

    mpr_dev_init((mpr_dev)dev, 1, NULL, 0);

    dev->prefix_len = strlen(name_prefix);
    dev->name = (char*)malloc(dev->prefix_len + 6);
    sprintf(dev->name, "%s.0", name_prefix);
    mpr_dev_start_servers(dev);

    if (!dev->servers[SERVER_UDP] || !dev->servers[SERVER_TCP]) {
        mpr_dev_free((mpr_dev)dev);
        return NULL;
    }

    dev->expr_stack = mpr_expr_stack_new();

    dev->ordinal_allocator.val = 1;
    dev->idmaps.active = (mpr_id_map*) malloc(sizeof(mpr_id_map));
    dev->idmaps.active[0] = 0;
    dev->num_sig_groups = 1;

    mpr_net_add_dev(mpr_graph_get_net(g), dev);

    dev->status = MPR_STATUS_STAGED;
    return (mpr_dev)dev;
}

/*! Free resources used by a mpr device. */
void mpr_dev_free(mpr_dev dev)
{
    mpr_graph gph;
    mpr_net net;
    mpr_local_dev ldev;
    mpr_list list;
    int i;
    RETURN_UNLESS(dev && dev->obj.is_local);
    if (!dev->obj.graph) {
        free(dev);
        return;
    }
    ldev = (mpr_local_dev)dev;
    gph = dev->obj.graph;
    net = mpr_graph_get_net(gph);

    /* free any queued graph messages without sending */
    mpr_net_free_msgs(net);

    /* remove OSC handlers associated with this device */
    mpr_net_remove_dev(net, ldev);

    /* remove local graph handlers here so they are not called when child objects are freed */
    if (!mpr_graph_get_owned(gph))
        mpr_graph_free_cbs(gph);

    /* remove subscribers */
    while (ldev->subscribers) {
        mpr_subscriber sub = ldev->subscribers;
        FUNC_IF(lo_address_free, sub->addr);
        ldev->subscribers = sub->next;
        free(sub);
    }

    /* free signals owned by this device */
    list = mpr_dev_get_sigs(dev, MPR_DIR_ANY);
    while (list) {
        mpr_local_sig sig = (mpr_local_sig)*list;
        list = mpr_list_get_next(list);
        if (sig->obj.is_local) {
            /* release active instances */
            for (i = 0; i < sig->idmap_len; i++) {
                if (sig->idmaps[i].inst)
                    mpr_sig_release_inst_internal(sig, i);
            }
        }
        mpr_sig_free((mpr_sig)sig);
    }

    if (ldev->registered) {
        /* A registered device must tell the network it is leaving. */
        NEW_LO_MSG(msg, ;)
        if (msg) {
            mpr_net_use_bus(net);
            lo_message_add_string(msg, mpr_dev_get_name(dev));
            mpr_net_add_msg(net, 0, MSG_LOGOUT, msg);
            mpr_net_send(net);
        }
    }

    /* Release links to other devices */
    list = mpr_dev_get_links(dev, MPR_DIR_UNDEFINED);
    while (list) {
        mpr_link link = (mpr_link)*list;
        list = mpr_list_get_next(list);
        _process_outgoing_maps(ldev);
        mpr_graph_remove_link(gph, link, MPR_OBJ_REM);
    }

    /* Release device id maps */
    for (i = 0; i < ldev->num_sig_groups; i++) {
        while (ldev->idmaps.active[i]) {
            mpr_id_map map = ldev->idmaps.active[i];
            ldev->idmaps.active[i] = map->next;
            free(map);
        }
    }
    free(ldev->idmaps.active);

    while (ldev->idmaps.reserve) {
        mpr_id_map map = ldev->idmaps.reserve;
        ldev->idmaps.reserve = map->next;
        free(map);
    }

    mpr_expr_stack_free(ldev->expr_stack);

    FUNC_IF(lo_server_free, ldev->servers[SERVER_UDP]);
    FUNC_IF(lo_server_free, ldev->servers[SERVER_TCP]);

    mpr_graph_remove_dev(gph, dev, MPR_OBJ_REM, 1);
    if (!mpr_graph_get_owned(gph))
        mpr_graph_free(gph);
}

void mpr_dev_free_mem(mpr_dev dev)
{
    FUNC_IF(free, dev->linked);
    FUNC_IF(free, dev->name);
}

void mpr_dev_on_registered(mpr_local_dev dev)
{
    int i;
    char *name;
    mpr_list qry;
    /* Add unique device id to locally-activated signal instances. */
    mpr_list sigs = mpr_dev_get_sigs((mpr_dev)dev, MPR_DIR_ANY);
    while (sigs) {
        mpr_local_sig sig = (mpr_local_sig)*sigs;
        sigs = mpr_list_get_next(sigs);
        for (i = 0; i < sig->idmap_len; i++) {
            mpr_id_map idmap = sig->idmaps[i].map;
            if (idmap && !(idmap->GID >> 32))
                idmap->GID |= dev->obj.id;
        }
        sig->obj.id |= dev->obj.id;
    }
    qry = mpr_graph_new_query(dev->obj.graph, 0, MPR_SIG, (void*)_cmp_qry_sigs,
                              "hi", dev->obj.id, MPR_DIR_ANY);
    mpr_tbl_set(dev->obj.props.synced, PROP(SIG), NULL, 1, MPR_LIST, qry,
                NON_MODIFIABLE | PROP_OWNED);
    dev->registered = 1;
    dev->ordinal = dev->ordinal_allocator.val;

    snprintf(dev->name + dev->prefix_len + 1, dev->prefix_len + 6, "%d", dev->ordinal);
    name = strdup(dev->name);
    free(dev->name);
    dev->name = name;

    dev->status = MPR_STATUS_READY;

    mpr_dev_get_name((mpr_dev)dev);

    /* Check if we have any staged maps */
    mpr_graph_cleanup(dev->obj.graph);
}

int mpr_dev_get_is_registered(mpr_dev dev)
{
    return !dev->obj.is_local || ((mpr_local_dev)dev)->registered;
}

MPR_INLINE static int check_types(const mpr_type *types, int len, mpr_type type, int vector_len)
{
    int i, vals = 0;
    RETURN_ARG_UNLESS(len >= vector_len, -1);
    for (i = 0; i < len; i++) {
        if (types[i] == type)
            ++vals;
        else if (types[i] != MPR_NULL)
            return -1;
    }
    return vals;
}

int mpr_dev_bundle_start(lo_timetag t, void *data)
{
    mpr_time_set(&ts, t);
    return 0;
}

/* Notes:
 * - Incoming signal values may be scalars or vectors, but much match the
 *   length of the target signal or mapping slot.
 * - Vectors are of homogeneous type (MPR_INT32, MPR_FLT or MPR_DBL) however
 *   individual elements may have no value (type MPR_NULL)
 * - A vector consisting completely of nulls indicates a signal instance release
 *   TODO: use more specific message for release?
 * - Updates to a specific signal instance are indicated using the label
 *   "@in" followed by a 64bit integer which uniquely identifies this
 *   instance within the network of libmapper devices
 * - Updates to specific "slots" of a convergent (i.e. multi-source) mapping
 *   are indicated using the label "@sl" followed by a single integer slot #
 * - Instance creation and release may also be triggered by expression
 *   evaluation. Refer to the document "Using Instanced Signals with Libmapper"
 *   for more information.
 */
int mpr_dev_handler(const char *path, const char *types, lo_arg **argv, int argc,
                    lo_message msg, void *data)
{
    mpr_local_sig sig = (mpr_local_sig)data;
    mpr_local_dev dev;
    mpr_sig_inst si;
    mpr_rtr rtr = mpr_net_get_rtr(mpr_graph_get_net(sig->obj.graph));
    int i, val_len = 0, vals, size, all;
    int idmap_idx, inst_idx, slot_idx = -1, map_manages_inst = 0;
    mpr_id GID = 0;
    mpr_id_map idmap;
    mpr_local_map map = 0;
    mpr_local_slot slot = 0;
    mpr_sig slot_sig = 0;
    float diff;

    TRACE_RETURN_UNLESS(sig && (dev = sig->dev), 0,
                        "error in mpr_dev_handler, cannot retrieve user data\n");
    TRACE_DEV_RETURN_UNLESS(sig->num_inst, 0, "signal '%s' has no instances.\n", sig->name);
    RETURN_ARG_UNLESS(argc, 0);

    /* We need to consider that there may be properties appended to the msg
     * check length and find properties if any */
    while (val_len < argc && types[val_len] != MPR_STR)
        ++val_len;
    i = val_len;
    while (i < argc) {
        /* Parse any attached properties (instance ids, slot number) */
        TRACE_DEV_RETURN_UNLESS(types[i] == MPR_STR, 0, "error in "
                                "mpr_dev_handler: unexpected argument type.\n")
        if ((strcmp(&argv[i]->s, "@in") == 0) && argc >= i + 2) {
            TRACE_DEV_RETURN_UNLESS(types[i+1] == MPR_INT64, 0, "error in "
                                    "mpr_dev_handler: bad arguments for 'instance' prop.\n")
            GID = argv[i+1]->i64;
            i += 2;
        }
        else if ((strcmp(&argv[i]->s, "@sl") == 0) && argc >= i + 2) {
            TRACE_DEV_RETURN_UNLESS(types[i+1] == MPR_INT32, 0, "error in "
                                    "mpr_dev_handler: bad arguments for 'slot' prop.\n")
            slot_idx = argv[i+1]->i32;
            i += 2;
        }
        else {
#ifdef DEBUG
            trace_dev(dev, "error in mpr_dev_handler: unknown property name '%s'.\n", &argv[i]->s);
#endif
            return 0;
        }
    }

    if (slot_idx >= 0) {
        /* retrieve mapping associated with this slot */
        slot = mpr_rtr_get_slot(rtr, sig, slot_idx);
        TRACE_DEV_RETURN_UNLESS(slot, 0, "error in mpr_dev_handler: slot %d not found.\n", slot_idx);
        slot_sig = mpr_slot_get_sig((mpr_slot)slot);
        map = (mpr_local_map)mpr_slot_get_map((mpr_slot)slot);
        TRACE_DEV_RETURN_UNLESS(map->status >= MPR_STATUS_READY, 0, "error in mpr_dev_handler: "
                                "mapping not yet ready.\n");
        if (map->expr && !map->is_local_only) {
            vals = check_types(types, val_len, slot_sig->type, slot_sig->len);
            map_manages_inst = mpr_expr_get_manages_inst(map->expr);
        }
        else {
            /* value has already been processed at source device */
            map = 0;
            vals = check_types(types, val_len, sig->type, sig->len);
        }
    }
    else
        vals = check_types(types, val_len, sig->type, sig->len);
    RETURN_ARG_UNLESS(vals >= 0, 0);

    /* TODO: optionally discard out-of-order messages
     * requires timebase sync for many-to-one mappings or local updates
     *    if (sig->discard_out_of_order && out_of_order(si->time, t))
     *        return 0;
     */

    if (GID) {
        idmap_idx = mpr_sig_get_idmap_with_GID(sig, GID, RELEASED_LOCALLY, ts, 0);
        if (idmap_idx < 0) {
            /* No instance found with this map – don't activate instance just to release it again */
            RETURN_ARG_UNLESS(vals && sig->dir == MPR_DIR_IN, 0);

            if (map_manages_inst && vals == slot_sig->len) {
                /* special case: do a dry-run to check whether this map will
                 * cause a release. If so, don't bother stealing an instance. */
                mpr_value *src;
                mpr_value_t v = {0, 0, 1, 0, 1};
                mpr_value_buffer_t b = {0, 0, -1};
                int slot_id = mpr_slot_get_id((mpr_slot)slot);
                b.samps = argv[0];
                v.inst = &b;
                v.vlen = val_len;
                v.type = slot_sig->type;
                src = alloca(map->num_src * sizeof(mpr_value));
                for (i = 0; i < map->num_src; i++)
                    src[i] = (i == slot_id) ? &v : 0;
                if (mpr_expr_eval(dev->expr_stack, map->expr, src, 0, 0, 0, 0, 0) & EXPR_RELEASE_BEFORE_UPDATE)
                    return 0;
            }

            /* otherwise try to init reserved/stolen instance with device map */
            idmap_idx = mpr_sig_get_idmap_with_GID(sig, GID, RELEASED_REMOTELY, ts, 1);
            TRACE_DEV_RETURN_UNLESS(idmap_idx >= 0, 0,
                                    "no instances available for GUID %"PR_MPR_ID" (1)\n", GID);
        }
        else if (sig->idmaps[idmap_idx].status & RELEASED_LOCALLY) {
            /* map was already released locally, we are only interested in release messages */
            if (0 == vals) {
                /* we can clear signal's reference to map */
                idmap = sig->idmaps[idmap_idx].map;
                sig->idmaps[idmap_idx].map = 0;
                mpr_dev_GID_decref(dev, sig->group, idmap);
            }
            return 0;
        }
        TRACE_DEV_RETURN_UNLESS(sig->idmaps[idmap_idx].inst, 0,
                                "error in mpr_dev_handler: missing instance!\n");
    }
    else {
        /* use the first available instance */
        for (i = 0; i < sig->num_inst; i++) {
            if (sig->inst[i]->active)
                break;
        }
        if (i >= sig->num_inst)
            i = 0;
        idmap_idx = mpr_sig_get_idmap_with_LID(sig, sig->inst[i]->id, RELEASED_REMOTELY, ts, 1);
        RETURN_ARG_UNLESS(idmap_idx >= 0, 0);
    }
    si = sig->idmaps[idmap_idx].inst;
    inst_idx = si->idx;
    diff = mpr_time_get_diff(ts, si->time);
    idmap = sig->idmaps[idmap_idx].map;

    size = mpr_type_get_size(map ? slot_sig->type : sig->type);
    if (vals == 0) {
        if (GID) {
            /* TODO: mark SLOT status as remotely released rather than idmap? */
            sig->idmaps[idmap_idx].status |= RELEASED_REMOTELY;
            mpr_dev_GID_decref(dev, sig->group, idmap);
            if (!sig->ephemeral) {
                /* clear signal's reference to idmap */
                mpr_dev_LID_decref(dev, sig->group, idmap);
                sig->idmaps[idmap_idx].map = 0;
                return 0;
            }
        }
        RETURN_ARG_UNLESS(sig->ephemeral && (!map || map->use_inst), 0);

        /* Try to release instance, but do not call mpr_rtr_process_sig() here, since we don't
         * know if the local signal instance will actually be released. */
        if (sig->dir == MPR_DIR_IN)
            mpr_sig_call_handler(sig, MPR_SIG_REL_UPSTRM, idmap->LID, 0, 0, ts, diff);
        else
            mpr_sig_call_handler(sig, MPR_SIG_REL_DNSTRM, idmap->LID, 0, 0, ts, diff);

        RETURN_ARG_UNLESS(map && MPR_LOC_DST == map->process_loc && sig->dir == MPR_DIR_IN, 0);

        /* Reset memory for corresponding source slot. */
        mpr_slot_reset_inst(slot, inst_idx);
        return 0;
    }
    else if (sig->dir == MPR_DIR_OUT && !sig->handler)
        return 0;

    /* Partial vector updates are not allowed in convergent maps since the slot value mirrors the
     * remote signal value. */
    if (map && vals != slot_sig->len) {
#ifdef DEBUG
        trace_dev(dev, "error in mpr_dev_handler: partial vector update "
                  "applied to convergent mapping slot.");
#endif
        return 0;
    }

    all = !GID;
    if (map) {
        /* Or if this signal slot is non-instanced but the map has other instanced
         * sources we will need to update all of the map instances. */
        all |= !map->use_inst || (map->num_src > 1 && map->num_inst > slot_sig->num_inst);
    }
    if (all)
        idmap_idx = 0;

    if (map) {
        for (; idmap_idx < sig->idmap_len; idmap_idx++) {
            /* check if map instance is active */
            if ((si = sig->idmaps[idmap_idx].inst) && si->active) {
                inst_idx = si->idx;
                /* Setting to local timestamp here */
                /* TODO: jitter mitigation etc. */
                if (mpr_slot_set_value(slot, inst_idx, argv[0], dev->time)) {
                    mpr_bitflags_set(map->updated_inst, inst_idx);
                    map->updated = 1;
                    dev->receiving = 1;
                }
            }
            if (!all)
                break;
        }
        return 0;
    }

    for (; idmap_idx < sig->idmap_len; idmap_idx++) {
        /* check if instance is active */
        if ((si = sig->idmaps[idmap_idx].inst) && si->active) {
            idmap = sig->idmaps[idmap_idx].map;
            for (i = 0; i < sig->len; i++) {
                if (types[i] == MPR_NULL)
                    continue;
                memcpy((char*)si->val + i * size, argv[i], size);
                mpr_bitflags_set(si->has_val_flags, i);
            }
            if (!mpr_bitflags_compare(si->has_val_flags, sig->vec_known, sig->len))
                si->has_val = 1;
            if (si->has_val) {
                memcpy(&si->time, &ts, sizeof(mpr_time));
                mpr_bitflags_unset(sig->updated_inst, si->idx);
                mpr_sig_call_handler(sig, MPR_SIG_UPDATE, idmap->LID, sig->len, si->val, ts, diff);
                /* Pass this update downstream if signal is an input and was not updated in handler. */
                if (!(sig->dir & MPR_DIR_OUT) && !mpr_bitflags_get(sig->updated_inst, si->idx)) {
                    mpr_rtr_process_sig(rtr, sig, idmap_idx, si->val, ts);
                    /* TODO: ensure update is propagated within this poll cycle */
                }
            }
        }
        if (!all)
            break;
    }
    return 0;
}

mpr_id mpr_dev_get_unused_sig_id(mpr_local_dev dev)
{
    int done = 0;
    mpr_id id;
    while (!done) {
        mpr_list l = mpr_dev_get_sigs((mpr_dev)dev, MPR_DIR_ANY);
        id = mpr_dev_generate_unique_id((mpr_dev)dev);
        /* check if input signal exists with this id */
        done = 1;
        while (l) {
            if ((*l)->id == id) {
                done = 0;
                mpr_list_free(l);
                break;
            }
            l = mpr_list_get_next(l);
        }
    }
    return id;
}

void mpr_dev_add_sig_methods(mpr_local_dev dev, mpr_local_sig sig)
{
    RETURN_UNLESS(sig && sig->obj.is_local);
    lo_server_add_method(dev->servers[SERVER_UDP], sig->path, NULL, mpr_dev_handler, (void*)sig);
    lo_server_add_method(dev->servers[SERVER_TCP], sig->path, NULL, mpr_dev_handler, (void*)sig);
    ++dev->n_output_callbacks;
}

void mpr_dev_remove_sig_methods(mpr_local_dev dev, mpr_local_sig sig)
{
    RETURN_UNLESS(sig && sig->obj.is_local);
    lo_server_del_method(dev->servers[SERVER_UDP], sig->path, NULL);
    lo_server_del_method(dev->servers[SERVER_TCP], sig->path, NULL);
    --dev->n_output_callbacks;
}

void mpr_dev_remove_sig(mpr_dev dev, mpr_sig sig)
{
    if (sig->dir & MPR_DIR_IN)
        --dev->num_inputs;
    if (sig->dir & MPR_DIR_OUT)
        --dev->num_outputs;
}

mpr_list mpr_dev_get_sigs(mpr_dev dev, mpr_dir dir)
{
    RETURN_ARG_UNLESS(dev, 0);
    return mpr_graph_new_query(dev->obj.graph, 1, MPR_SIG, (void*)_cmp_qry_sigs,
                               "hi", dev->obj.id, dir);
}

mpr_sig mpr_dev_get_sig_by_name(mpr_dev dev, const char *sig_name)
{
    mpr_list sigs;
    RETURN_ARG_UNLESS(dev && sig_name, 0);
    sigs = mpr_graph_get_list(dev->obj.graph, MPR_SIG);
    while (sigs) {
        mpr_sig sig = (mpr_sig)*sigs;
        if ((sig->dev == dev) && strcmp(sig->name, mpr_path_skip_slash(sig_name))==0)
            return sig;
        sigs = mpr_list_get_next(sigs);
    }
    return 0;
}

static int _cmp_qry_maps(const void *context_data, mpr_map map)
{
    mpr_id dev_id = *(mpr_id*)context_data;
    mpr_dir dir = *(int*)((char*)context_data + sizeof(mpr_id));
    mpr_sig sig;
    int i;
    if (dir == MPR_DIR_BOTH) {
        sig = mpr_slot_get_sig(map->dst);
        RETURN_ARG_UNLESS(sig->dev->obj.id == dev_id, 0);
        for (i = 0; i < map->num_src; i++) {
            sig = mpr_slot_get_sig(map->src[i]);
            RETURN_ARG_UNLESS(sig->dev->obj.id == dev_id, 0);
        }
        return 1;
    }
    if (dir & MPR_DIR_OUT) {
        for (i = 0; i < map->num_src; i++) {
            sig = mpr_slot_get_sig(map->src[i]);
            RETURN_ARG_UNLESS(sig->dev->obj.id != dev_id, 1);
        }
    }
    if (dir & MPR_DIR_IN) {
        sig = mpr_slot_get_sig(map->dst);
        RETURN_ARG_UNLESS(sig->dev->obj.id != dev_id, 1);
    }
    return 0;
}

mpr_list mpr_dev_get_maps(mpr_dev dev, mpr_dir dir)
{
    RETURN_ARG_UNLESS(dev, 0);
    return mpr_graph_new_query(dev->obj.graph, 1, MPR_MAP, (void*)_cmp_qry_maps,
                               "hi", dev->obj.id, dir);
}

static int _cmp_qry_links(const void *context_data, mpr_link link)
{
    mpr_id dev_id = *(mpr_id*)context_data;
    mpr_dir dir = *(int*)((char*)context_data + sizeof(mpr_id));
    mpr_dev dev = mpr_link_get_dev(link, 0);
    if (dev->obj.id == dev_id) {
        return MPR_DIR_UNDEFINED == dir ? 1 : mpr_link_get_has_maps(link, dir);
    }
    dev = mpr_link_get_dev(link, 1);
    if (dev->obj.id == dev_id) {
        switch (dir) {
            case MPR_DIR_ANY:
            case MPR_DIR_BOTH:  return mpr_link_get_has_maps(link, dir);
            case MPR_DIR_IN:    return mpr_link_get_has_maps(link, MPR_DIR_OUT);
            case MPR_DIR_OUT:   return mpr_link_get_has_maps(link, MPR_DIR_IN);
            default:            return 1;
        }
    }
    return 0;
}

mpr_list mpr_dev_get_links(mpr_dev dev, mpr_dir dir)
{
    RETURN_ARG_UNLESS(dev, 0);
    return mpr_graph_new_query(dev->obj.graph, 1, MPR_LINK, (void*)_cmp_qry_links,
                               "hi", dev->obj.id, dir);
}

mpr_link mpr_dev_get_link_by_remote(mpr_local_dev dev, mpr_dev remote)
{
    mpr_list links;
    RETURN_ARG_UNLESS(dev, 0);
    links = mpr_graph_get_list(dev->obj.graph, MPR_LINK);
    while (links) {
        mpr_link link = (mpr_link)*links;
        if (mpr_link_get_dev(link, 0) == (mpr_dev)dev && mpr_link_get_dev(link, 1) == remote)
            return link;
        if (mpr_link_get_dev(link, 1) == (mpr_dev)dev && mpr_link_get_dev(link, 0) == remote)
            return link;
        links = mpr_list_get_next(links);
    }
    return 0;
}

/* TODO: handle interrupt-driven updates that omit call to this function */
MPR_INLINE static void _process_incoming_maps(mpr_local_dev dev)
{
    mpr_graph graph;
    mpr_list maps;
    RETURN_UNLESS(dev->receiving);
    graph = dev->obj.graph;
    /* process and send updated maps */
    /* TODO: speed this up! */
    dev->receiving = 0;
    maps = mpr_graph_get_list(graph, MPR_MAP);
    while (maps) {
        mpr_local_map map = *(mpr_local_map*)maps;
        maps = mpr_list_get_next(maps);
        if (map->obj.is_local && map->updated && map->expr && !map->muted)
            mpr_map_receive(map, dev->time);
    }
}

/* TODO: handle interrupt-driven updates that omit call to this function */
MPR_INLINE static int _process_outgoing_maps(mpr_local_dev dev)
{
    int msgs = 0;
    mpr_list list;
    mpr_graph graph;
    RETURN_ARG_UNLESS(dev->sending, 0);

    graph = dev->obj.graph;
    /* process and send updated maps */
    /* TODO: speed this up! */
    list = mpr_graph_get_list(graph, MPR_MAP);
    while (list) {
        mpr_local_map map = *(mpr_local_map*)list;
        list = mpr_list_get_next(list);
        if (map->obj.is_local && map->updated && map->expr && !map->muted)
            mpr_map_send(map, dev->time);
    }
    dev->sending = 0;
    list = mpr_graph_get_list(graph, MPR_LINK);
    while (list) {
        msgs += mpr_link_process_bundles((mpr_link)*list, dev->time, 0);
        list = mpr_list_get_next(list);
    }
    return msgs ? 1 : 0;
}

void mpr_dev_update_maps(mpr_dev dev) {
    RETURN_UNLESS(dev && dev->obj.is_local);
    ((mpr_local_dev)dev)->time_is_stale = 1;
    if (!((mpr_local_dev)dev)->polling)
        _process_outgoing_maps((mpr_local_dev)dev);
}

int mpr_dev_poll(mpr_dev dev, int block_ms)
{
    int admin_count = 0, device_count = 0, status[4];
    mpr_local_dev ldev = (mpr_local_dev)dev;
    mpr_net net;
    RETURN_ARG_UNLESS(dev && dev->obj.is_local, 0);
    net = mpr_graph_get_net(dev->obj.graph);
    mpr_net_poll(net);
    mpr_graph_housekeeping(dev->obj.graph);

    if (!ldev->registered) {
        if (lo_servers_recv_noblock(net->servers, status, 2, block_ms)) {
            admin_count = (status[0] > 0) + (status[1] > 0);
            net->msgs_recvd |= admin_count;
        }
        ldev->bundle_idx = 1;
        return admin_count;
    }

    ldev->polling = 1;
    ldev->time_is_stale = 1;
    mpr_dev_get_time(dev);
    _process_outgoing_maps(ldev);
    ldev->polling = 0;

    if (!block_ms) {
        if (lo_servers_recv_noblock(ldev->servers, status, 4, 0)) {
            admin_count = (status[0] > 0) + (status[1] > 0);
            device_count = (status[2] > 0) + (status[3] > 0);
            net->msgs_recvd |= admin_count;
        }
    }
    else {
        double then = mpr_get_current_time();
        int left_ms = block_ms, elapsed, checked_admin = 0;
        while (left_ms > 0) {
            /* set timeout to a maximum of 100ms */
            if (left_ms > 100)
                left_ms = 100;
            ldev->polling = 1;
            if (lo_servers_recv_noblock(ldev->servers, status, 4, left_ms)) {
                admin_count += (status[0] > 0) + (status[1] > 0);
                device_count += (status[2] > 0) + (status[3] > 0);
            }
            /* check if any signal update bundles need to be sent */
            _process_incoming_maps(ldev);
            _process_outgoing_maps(ldev);
            ldev->polling = 0;

            elapsed = (mpr_get_current_time() - then) * 1000;
            if ((elapsed - checked_admin) > 100) {
                mpr_net_poll(net);
                mpr_graph_housekeeping(dev->obj.graph);
                checked_admin = elapsed;
            }
            left_ms = block_ms - elapsed;
        }
    }

    /* When done, or if non-blocking, check for remaining messages up to a
     * proportion of the number of input signals. Arbitrarily choosing 1 for
     * now, but perhaps could be a heuristic based on a recent number of
     * messages per channel per poll. */
    while (device_count < (dev->num_inputs + ldev->n_output_callbacks)*1
           && (lo_servers_recv_noblock(ldev->servers, &status[2], 2, 0)))
        device_count += (status[2] > 0) + (status[3] > 0);

    /* process incoming maps */
    ldev->polling = 1;
    _process_incoming_maps(ldev);
    ldev->polling = 0;

    if (dev->obj.props.synced->dirty && mpr_dev_get_is_ready(dev) && ldev->subscribers) {
        /* inform device subscribers of changed properties */
        mpr_net_use_subscribers(net, ldev, MPR_DEV);
        mpr_dev_send_state(dev, MSG_DEV);
    }

    net->msgs_recvd |= admin_count;
    return admin_count + device_count;
}

#ifdef HAVE_LIBPTHREAD
static void *device_thread_func(void *data)
{
    mpr_thread_data td = (mpr_thread_data)data;
    while (td->is_active) {
        mpr_dev_poll((mpr_dev)td->object, 100);
    }
    td->is_done = 1;
    pthread_exit(NULL);
    return 0;
}
#endif

#ifdef HAVE_WIN32_THREADS
static unsigned __stdcall device_thread_func(void *data)
{
    mpr_thread_data td = (mpr_thread_data)data;
    while (td->is_active) {
        mpr_dev_poll((mpr_dev)td->object, 100);
    }
    td->is_done = 1;
    _endthread();
    return 0;
}
#endif

int mpr_dev_start_polling(mpr_dev dev)
{
    mpr_thread_data td;
    int result = 0;
    RETURN_ARG_UNLESS(dev && dev->obj.is_local, 0);
    if (((mpr_local_dev)dev)->thread_data)
        return 0;

    td = (mpr_thread_data)malloc(sizeof(mpr_thread_data_t));
    td->object = (mpr_obj)dev;
    td->is_active = 1;


#ifdef HAVE_LIBPTHREAD
    result = -pthread_create(&(td->thread), 0, device_thread_func, td);
#else
#ifdef HAVE_WIN32_THREADS
    if (!(td->thread = (HANDLE)_beginthreadex(NULL, 0, &device_thread_func, td, 0, NULL)))
        result = -1;
#else
    printf("error: threading is not available.\n");
#endif /* HAVE_WIN32_THREADS */
#endif /* HAVE_LIBPTHREAD */

    if (result) {
        printf("Device error: couldn't create thread.\n");
        free(td);
    }
    else {
        ((mpr_local_dev)dev)->thread_data = td;
    }
    return result;
}

int mpr_dev_stop_polling(mpr_dev dev)
{
    mpr_thread_data td;
    int result = 0;
    RETURN_ARG_UNLESS(dev && dev->obj.is_local, 0);
    td = ((mpr_local_dev)dev)->thread_data;
    if (!td || !td->is_active)
        return 0;
    td->is_active = 0;

#ifdef HAVE_LIBPTHREAD
    result = pthread_join(td->thread, NULL);
    if (result) {
        printf("Device error: failed to stop thread (pthread_join).\n");
        return -result;
    }
#else
#ifdef HAVE_WIN32_THREADS
    result = WaitForSingleObject(td->thread, INFINITE);
    CloseHandle(td->thread);
    td->thread = NULL;

    if (0 != result) {
        printf("Device error: failed to join thread (WaitForSingleObject).\n");
        return -1;
    }
#else
    printf("error: threading is not available.\n");
#endif /* HAVE_WIN32_THREADS */
#endif /* HAVE_LIBPTHREAD */

    free(((mpr_local_dev)dev)->thread_data);
    ((mpr_local_dev)dev)->thread_data = 0;
    return result;
}

mpr_time mpr_dev_get_time(mpr_dev dev)
{
    RETURN_ARG_UNLESS(dev && dev->obj.is_local, MPR_NOW);
    if (((mpr_local_dev)dev)->time_is_stale)
        mpr_dev_set_time(dev, MPR_NOW);
    return ((mpr_local_dev)dev)->time;
}

void mpr_dev_set_time(mpr_dev dev, mpr_time time)
{
    RETURN_UNLESS(dev && dev->obj.is_local
                  && memcmp(&time, &((mpr_local_dev)dev)->time, sizeof(mpr_time)));
    mpr_time_set(&((mpr_local_dev)dev)->time, time);
    ((mpr_local_dev)dev)->time_is_stale = 0;
    if (!((mpr_local_dev)dev)->polling)
        _process_outgoing_maps((mpr_local_dev)dev);
}

void mpr_dev_reserve_idmap(mpr_local_dev dev)
{
    mpr_id_map map;
    map = (mpr_id_map)calloc(1, sizeof(mpr_id_map_t));
    map->next = dev->idmaps.reserve;
    dev->idmaps.reserve = map;
}

int mpr_local_dev_get_num_idmaps(mpr_local_dev dev, int active)
{
    int count = 0;
    mpr_id_map *id_map = active ? &(dev)->idmaps.active[0] : &(dev)->idmaps.reserve;
    while (*id_map) {
        ++count;
        id_map = &(*id_map)->next;
    }
    return count;
}

#ifdef DEBUG
void mpr_local_dev_print_idmaps(mpr_local_dev dev)
{
    printf("ID MAPS for %s:\n", dev->name);
    mpr_id_map *map = &dev->idmaps.active[0];
    while (*map) {
        mpr_id_map m = *map;
        printf("  %p: %"PR_MPR_ID" (%d) -> %"PR_MPR_ID" (%d)\n",
               m, m->LID, m->LID_refcount, m->GID, m->GID_refcount);
        map = &(*map)->next;
    }
}
#endif

mpr_id_map mpr_dev_add_idmap(mpr_local_dev dev, int group, mpr_id LID, mpr_id GID)
{
    mpr_id_map map;
    if (!dev->idmaps.reserve)
        mpr_dev_reserve_idmap(dev);
    map = dev->idmaps.reserve;
    map->LID = LID;
    map->GID = GID ? GID : mpr_dev_generate_unique_id((mpr_dev)dev);
    trace_dev(dev, "mpr_dev_add_idmap(%s) %"PR_MPR_ID" -> %"PR_MPR_ID"\n", dev->name, LID, map->GID);
    map->LID_refcount = 1;
    map->GID_refcount = 0;
    dev->idmaps.reserve = map->next;
    map->next = dev->idmaps.active[group];
    dev->idmaps.active[group] = map;
#ifdef DEBUG
    mpr_local_dev_print_idmaps(dev);
#endif
    return map;
}

static void mpr_dev_remove_idmap(mpr_local_dev dev, int group, mpr_id_map rem)
{
    mpr_id_map *map = &dev->idmaps.active[group];
    trace_dev(dev, "mpr_dev_remove_idmap(%s) %"PR_MPR_ID" -> %"PR_MPR_ID"\n",
              dev->name, rem->LID, rem->GID);
    while (*map) {
        if ((*map) == rem) {
            *map = (*map)->next;
            rem->next = dev->idmaps.reserve;
            dev->idmaps.reserve = rem;
            break;
        }
        map = &(*map)->next;
    }
#ifdef DEBUG
    mpr_local_dev_print_idmaps(dev);
#endif
}

int mpr_dev_LID_decref(mpr_local_dev dev, int group, mpr_id_map map)
{
    trace_dev(dev, "mpr_dev_LID_decref(%s) %"PR_MPR_ID" -> %"PR_MPR_ID"\n",
              dev->name, map->LID, map->GID);
    --map->LID_refcount;
    trace_dev(dev, "  refcounts: {LID:%d, GID:%d}\n", map->LID_refcount, map->GID_refcount);
    if (map->LID_refcount <= 0) {
        map->LID_refcount = 0;
        if (map->GID_refcount <= 0) {
            mpr_dev_remove_idmap(dev, group, map);
            return 1;
        }
    }
    return 0;
}

int mpr_dev_GID_decref(mpr_local_dev dev, int group, mpr_id_map map)
{
    trace_dev(dev, "mpr_dev_GID_decref(%s) %"PR_MPR_ID" -> %"PR_MPR_ID"\n",
              dev->name, map->LID, map->GID);
    --map->GID_refcount;
    trace_dev(dev, "  refcounts: {LID:%d, GID:%d}\n", map->LID_refcount, map->GID_refcount);
    if (map->GID_refcount <= 0) {
        map->GID_refcount = 0;
        if (map->LID_refcount <= 0) {
            mpr_dev_remove_idmap(dev, group, map);
            return 1;
        }
    }
    return 0;
}

mpr_id_map mpr_dev_get_idmap_by_LID(mpr_local_dev dev, int group, mpr_id LID)
{
    mpr_id_map map = dev->idmaps.active[group];
    while (map) {
        if (map->LID == LID)
            return map;
        map = map->next;
    }
    return 0;
}

mpr_id_map mpr_dev_get_idmap_by_GID(mpr_local_dev dev, int group, mpr_id GID)
{
    mpr_id_map map = dev->idmaps.active[group];
    while (map) {
        if (map->GID == GID)
            return map;
        map = map->next;
    }
    return 0;
}

/* Internal LibLo error handler */
static void handler_error(int num, const char *msg, const char *where)
{
    trace_net("[libmapper] liblo server error %d in path %s: %s\n", num, where, msg);
}

static void mpr_dev_start_servers(mpr_local_dev dev)
{
    int portnum;
    char port[16], *pport = 0, *url, *host;
    if (!dev->servers[SERVER_UDP] && !dev->servers[SERVER_TCP]) {
        while (!(dev->servers[SERVER_UDP] = lo_server_new(pport, handler_error)))
            pport = 0;
        snprintf(port, 16, "%d", lo_server_get_port(dev->servers[SERVER_UDP]));
        pport = port;
        while (!(dev->servers[SERVER_TCP] = lo_server_new_with_proto(pport, LO_TCP, handler_error)))
            pport = 0;

        /* Disable liblo message queueing */
        lo_server_enable_queue(dev->servers[SERVER_UDP], 0, 1);
        lo_server_enable_queue(dev->servers[SERVER_TCP], 0, 1);

        /* Add bundle handlers */
        lo_server_add_bundle_handlers(dev->servers[SERVER_UDP], mpr_dev_bundle_start, NULL, (void*)dev);
        lo_server_add_bundle_handlers(dev->servers[SERVER_TCP], mpr_dev_bundle_start, NULL, (void*)dev);
    }

    portnum = lo_server_get_port(dev->servers[SERVER_UDP]);
    mpr_tbl_set(dev->obj.props.synced, PROP(PORT), NULL, 1, MPR_INT32, &portnum, NON_MODIFIABLE);

    trace_dev(dev, "bound to UDP port %i\n", portnum);
    trace_dev(dev, "bound to TCP port %i\n", lo_server_get_port(dev->servers[SERVER_TCP]));

    url = lo_server_get_url(dev->servers[SERVER_UDP]);
    host = lo_url_get_hostname(url);
    mpr_tbl_set(dev->obj.props.synced, PROP(HOST), NULL, 1, MPR_STR, host, NON_MODIFIABLE);
    free(host);
    free(url);

    memcpy(dev->servers + 2, mpr_net_get_servers(mpr_graph_get_net(dev->obj.graph)),
           sizeof(lo_server) * 2);
}

/*! Probe the network to see if a device's proposed name.ordinal is available. */
void mpr_local_dev_probe_name(mpr_local_dev dev, mpr_net net)
{
    int i;

    /* reset collisions and hints */
    dev->ordinal_allocator.collision_count = 0;
    dev->ordinal_allocator.count_time = mpr_get_current_time();
    for (i = 0; i < 8; i++)
        dev->ordinal_allocator.hints[i] = 0;

    snprintf(dev->name + dev->prefix_len + 1, dev->prefix_len + 6, "%d", dev->ordinal_allocator.val);
    trace_dev(dev, "probing name '%s'\n", dev->name);

    /* Calculate an id from the name and store it in id.val */
    mpr_obj_set_id(&dev->obj, (mpr_id) crc32(0L, (const Bytef *)dev->name, strlen(dev->name)) << 32);

    mpr_net_send_name_probe(net, dev->name);
}

/* Extract the ordinal from a device name in the format: <name>.<ordinal> */
static int extract_ordinal(char *name) {
    int ordinal;
    char *s = name;
    RETURN_ARG_UNLESS(s = strrchr(s, '.'), -1);
    ordinal = atoi(s+1);
    *s = 0;
    return ordinal;
}

void mpr_local_dev_handler_name(mpr_local_dev dev, const char *name,
                                int temp_id, int random_id, int hint)
{
    mpr_net net = mpr_graph_get_net(mpr_obj_get_graph((mpr_obj)dev));
    int ordinal, diff;

#ifdef DEBUG
    if (hint)
        {trace_dev(dev, "received name %s %i %i\n", name, temp_id, hint);}
    else
        {trace_dev(dev, "received name %s\n", name);}
#endif

    if (dev->ordinal_allocator.locked) {
        /* extract_ordinal function replaces '.' with NULL */
        ordinal = extract_ordinal((char*)name);
        RETURN_UNLESS(ordinal >= 0);

        /* If device name matches */
        if (strlen(name) == dev->prefix_len && 0 == strncmp(name, dev->name, dev->prefix_len)) {
            /* if id is locked and registered id is within my block, store it */
            diff = ordinal - dev->ordinal_allocator.val - 1;
            if (diff >= 0 && diff < 8)
                dev->ordinal_allocator.hints[diff] = -1;
            if (hint) {
                /* if suggested id is within my block, store timestamp */
                diff = hint - dev->ordinal_allocator.val - 1;
                if (diff >= 0 && diff < 8)
                    dev->ordinal_allocator.hints[diff] = mpr_get_current_time();
            }
        }
    }
    else {
        mpr_id id = (mpr_id) crc32(0L, (const Bytef *)name, strlen(name)) << 32;
        if (id == mpr_obj_get_id((mpr_obj)dev)) {
            if (temp_id < random_id) {
                /* Count ordinal collisions. */
                ++dev->ordinal_allocator.collision_count;
                dev->ordinal_allocator.count_time = mpr_get_current_time();
            }
            else if (temp_id == random_id && hint > 0 && hint != dev->ordinal_allocator.val) {
                dev->ordinal_allocator.val = hint;
                mpr_local_dev_probe_name(dev, net);
            }
        }
    }
}

void mpr_local_dev_handler_name_probe(mpr_local_dev dev, char *name, int temp_id,
                                     int random_id, mpr_id id)
{
    int i;
    double current_time;
    if (id != mpr_obj_get_id(&dev->obj))
        return;

    trace_dev(dev, "name probe match %s %i \n", name, temp_id);
    current_time = mpr_get_current_time();
    if (dev->ordinal_allocator.locked || temp_id > random_id) {
        mpr_net net = mpr_graph_get_net(mpr_obj_get_graph(&dev->obj));
        for (i = 0; i < 8; i++) {
            if (   dev->ordinal_allocator.hints[i] >= 0
                && (current_time - dev->ordinal_allocator.hints[i]) > 2.0) {
                /* reserve suggested ordinal */
                dev->ordinal_allocator.hints[i] = current_time;
                break;
            }
        }
        /* Send /registered message with an ordinal hint */
        mpr_net_send_name_registered(net, name, temp_id, dev->ordinal_allocator.val + i + 1);
    }
    else {
        dev->ordinal_allocator.collision_count += 1;
        dev->ordinal_allocator.count_time = current_time;
        if (temp_id == random_id)
            dev->ordinal_allocator.online = 1;
    }
}

const char *mpr_dev_get_name(mpr_dev dev)
{
    return dev->name;
}

int mpr_dev_get_is_ready(mpr_dev dev)
{
    return dev ? dev->status >= MPR_STATUS_READY : 0;
}

mpr_id mpr_dev_generate_unique_id(mpr_dev dev)
{
    mpr_id id;
    RETURN_ARG_UNLESS(dev, 0);
    id = mpr_graph_generate_unique_id(dev->obj.graph);
    if (dev->obj.is_local && ((mpr_local_dev)dev)->registered)
        id |= dev->obj.id;
    return id;
}

void mpr_dev_send_state(mpr_dev dev, net_msg_t cmd)
{
    mpr_net net = mpr_graph_get_net(dev->obj.graph);
    NEW_LO_MSG(msg, return);

    /* device name */
    lo_message_add_string(msg, mpr_dev_get_name((mpr_dev)dev));

    /* properties */
    mpr_tbl_add_to_msg(dev->obj.is_local ? dev->obj.props.synced : 0, dev->obj.props.staged, msg);

    if (cmd == MSG_DEV_MOD) {
        char str[1024];
        snprintf(str, 1024, "/%s/modify", dev->name);
        mpr_net_add_msg(net, str, 0, msg);
        mpr_net_send(net);
    }
    else
        mpr_net_add_msg(net, 0, cmd, msg);

    dev->obj.props.synced->dirty = 0;
}

int mpr_dev_add_link(mpr_dev dev, mpr_dev rem)
{
    int i, found = 0;
    for (i = 0; i < dev->num_linked; i++) {
        if (dev->linked[i] && dev->linked[i]->obj.id == rem->obj.id) {
            found = 0x01;
            break;
        }
    }
    if (!found) {
        i = ++dev->num_linked;
        dev->linked = realloc(dev->linked, i * sizeof(mpr_dev));
        dev->linked[i-1] = rem;
    }

    for (i = 0; i < rem->num_linked; i++) {
        if (rem->linked[i] && rem->linked[i]->obj.id == dev->obj.id) {
            found |= 0x10;
            break;
        }
    }
    if (!(found & 0x10)) {
        i = ++rem->num_linked;
        rem->linked = realloc(rem->linked, i * sizeof(mpr_dev));
        rem->linked[i-1] = dev;
    }
    return !found;
}

void mpr_dev_remove_link(mpr_dev dev, mpr_dev rem)
{
    int i, j;
    for (i = 0; i < dev->num_linked; i++) {
        if (!dev->linked[i] || dev->linked[i]->obj.id != rem->obj.id)
            continue;
        for (j = i+1; j < dev->num_linked; j++)
            dev->linked[j-1] = dev->linked[j];
        --dev->num_linked;
        dev->linked = realloc(dev->linked, dev->num_linked * sizeof(mpr_dev));
        dev->obj.props.synced->dirty = 1;
        break;
    }
    for (i = 0; i < rem->num_linked; i++) {
        if (!rem->linked[i] || rem->linked[i]->obj.id != dev->obj.id)
            continue;
        for (j = i+1; j < rem->num_linked; j++)
            rem->linked[j-1] = rem->linked[j];
        --rem->num_linked;
        rem->linked = realloc(rem->linked, rem->num_linked * sizeof(mpr_dev));
        rem->obj.props.synced->dirty = 1;
        break;
    }
}

static int mpr_dev_update_linked(mpr_dev dev, mpr_msg_atom a)
{
    int i, j, updated = 0, num = mpr_msg_atom_get_len(a);
    lo_arg **link_list = mpr_msg_atom_get_values(a);
    if (link_list && *link_list) {
        const char *name;
        if (num == 1 && strcmp(&link_list[0]->s, "none")==0)
            num = 0;

        /* Remove any old links that are missing */
        for (i = 0; ; i++) {
            int found = 0;
            if (i >= dev->num_linked)
                break;
            for (j = 0; j < num; j++) {
                name = &link_list[j]->s;
                name = name[0] == '/' ? name + 1 : name;
                if (0 == strcmp(name, dev->linked[i]->name)) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (j = i+1; j < dev->num_linked; j++)
                    dev->linked[j-1] = dev->linked[j];
                --dev->num_linked;
                ++updated;
            }
        }
        if (updated)
            dev->linked = realloc(dev->linked, dev->num_linked * sizeof(mpr_dev));
        /* Add any new links */
        for (i = 0; i < num; i++) {
            mpr_dev rem;
            if ((rem = mpr_graph_add_dev(dev->obj.graph, &link_list[i]->s, 0, 1)))
                updated += mpr_dev_add_link(dev, rem);
        }
    }
    return updated;
}

/*! Update information about a device record based on message properties. */
int mpr_dev_set_from_msg(mpr_dev dev, mpr_msg m)
{
    int i, num, updated = 0;
    RETURN_ARG_UNLESS(m, 0);
    num = mpr_msg_get_num_atoms(m);
    for (i = 0; i < num; i++) {
        mpr_msg_atom a = mpr_msg_get_atom(m, i);
        switch (MASK_PROP_BITFLAGS(mpr_msg_atom_get_prop(a))) {
            case PROP(LINKED): {
                if (!dev->obj.is_local)
                    updated += mpr_dev_update_linked(dev, a);
                break;
            }
            default:
                updated += mpr_tbl_set_from_atom(dev->obj.props.synced, a, REMOTE_MODIFY);
                break;
        }
    }
    return updated;
}

static int mpr_dev_send_sigs(mpr_local_dev dev, mpr_dir dir)
{
    mpr_list l = mpr_dev_get_sigs((mpr_dev)dev, dir);
    while (l) {
        mpr_sig_send_state((mpr_sig)*l, MSG_SIG);
        l = mpr_list_get_next(l);
    }
    return 0;
}

int mpr_dev_send_maps(mpr_local_dev dev, mpr_dir dir, int msg)
{
    mpr_list l = mpr_dev_get_maps((mpr_dev)dev, dir);
    while (l) {
        mpr_map m = (mpr_map)*l;
        int i, ready = 1;
        mpr_sig sig = mpr_slot_get_sig(m->dst);
        l = mpr_list_get_next(l);
        if (sig->obj.is_local && !((mpr_local_dev)sig->dev)->registered)
            continue;
        for (i = 0; i < m->num_src; i++) {
            sig = mpr_slot_get_sig(m->src[i]);
            if (sig->obj.is_local && !((mpr_local_dev)sig->dev)->registered) {
                ready = 0;
                break;
            }
        }
        if (ready)
            mpr_map_send_state(m, -1, msg);
    }
    return 0;
}

int mpr_dev_get_is_subscribed(mpr_dev dev)
{
    return dev->subscribed != 0;
}

void mpr_dev_set_is_subscribed(mpr_dev dev, int subscribed)
{
    dev->subscribed = (subscribed != 0);
}

/* Add/renew/remove a subscription. */
void mpr_dev_manage_subscriber(mpr_local_dev dev, lo_address addr, int flags,
                               int timeout_sec, int revision)
{
    mpr_time t;
    mpr_net net;
    mpr_subscriber *s = &dev->subscribers;
    const char *ip = lo_address_get_hostname(addr);
    const char *port = lo_address_get_port(addr);
    RETURN_UNLESS(ip && port);
    mpr_time_set(&t, MPR_NOW);

    if (timeout_sec >= 0) {
        while (*s) {
            const char *s_ip = lo_address_get_hostname((*s)->addr);
            const char *s_port = lo_address_get_port((*s)->addr);
            if (strcmp(ip, s_ip)==0 && strcmp(port, s_port)==0) {
                /* subscriber already exists */
                if (!flags || !timeout_sec) {
                    /* remove subscription */
                    mpr_subscriber temp = *s;
                    int prev_flags = temp->flags;
                    trace_dev(dev, "removing subscription from %s:%s\n", s_ip, s_port);
                    *s = temp->next;
                    FUNC_IF(lo_address_free, temp->addr);
                    free(temp);
                    RETURN_UNLESS(flags && (flags &= ~prev_flags));
                }
                else {
                    /* reset timeout */
                    int temp = flags;
    #ifdef DEBUG
                    trace_dev(dev, "renewing subscription from %s:%s for %d seconds with flags ",
                              s_ip, s_port, timeout_sec);
                    print_subscription_flags(flags);
    #endif
                    (*s)->lease_exp = t.sec + timeout_sec;
                    flags &= ~(*s)->flags;
                    (*s)->flags = temp;
                }
                break;
            }
            s = &(*s)->next;
        }
    }

    RETURN_UNLESS(flags);

    if (!(*s) && timeout_sec > 0) {
        /* add new subscriber */
#ifdef DEBUG
        trace_dev(dev, "adding new subscription from %s:%s with flags ", ip, port);
        print_subscription_flags(flags);
#endif
        mpr_subscriber sub = malloc(sizeof(struct _mpr_subscriber));
        sub->addr = lo_address_new(ip, port);
        sub->lease_exp = t.sec + timeout_sec;
        sub->flags = flags;
        sub->next = dev->subscribers;
        dev->subscribers = sub;
    }

    /* bring new subscriber up to date */
    net = mpr_graph_get_net(dev->obj.graph);
    mpr_net_use_mesh(net, addr);
    mpr_dev_send_state((mpr_dev)dev, MSG_DEV);
    mpr_net_send(net);

    if (flags & MPR_SIG) {
        mpr_dir dir = 0;
        if (flags & MPR_SIG_IN)
            dir |= MPR_DIR_IN;
        if (flags & MPR_SIG_OUT)
            dir |= MPR_DIR_OUT;
        mpr_net_use_mesh(net, addr);
        mpr_dev_send_sigs(dev, dir);
        mpr_net_send(net);
    }
    if (flags & MPR_MAP) {
        mpr_dir dir = 0;
        if (flags & MPR_MAP_IN)
            dir |= MPR_DIR_IN;
        if (flags & MPR_MAP_OUT)
            dir |= MPR_DIR_OUT;
        mpr_net_use_mesh(net, addr);
        mpr_dev_send_maps(dev, dir, MSG_MAPPED);
        mpr_net_send(net);
    }
}

int mpr_dev_check_synced(mpr_dev dev, mpr_time time)
{
    return !dev->synced.sec || (dev->synced.sec > time.sec);
}

void mpr_dev_set_synced(mpr_dev dev, mpr_time time)
{
    mpr_time_set(&dev->synced, time);
}

int mpr_dev_has_local_link(mpr_dev dev)
{
    int i;
    for (i = 0; i < dev->num_linked; i++) {
        if (dev->linked[i] && mpr_obj_get_is_local((mpr_obj)dev->linked[i]))
            return 1;
    }
    return 0;
}

lo_server mpr_local_dev_get_server(mpr_local_dev dev, dev_server_t idx)
{
    return dev->servers[idx];
}

int mpr_local_dev_get_bundle_idx(mpr_local_dev dev)
{
    return dev->bundle_idx % NUM_BUNDLES;
}

mpr_expr_stack mpr_local_dev_get_expr_stack(mpr_local_dev dev)
{
    return dev->expr_stack;
}

void mpr_local_dev_set_sending(mpr_local_dev dev)
{
    dev->sending = 1;
}

int mpr_local_dev_has_subscribers(mpr_local_dev dev)
{
    return dev->subscribers != 0;
}

void mpr_local_dev_send_to_subscribers(mpr_local_dev dev, lo_bundle bundle,
                                       int msg_type, lo_server from)
{
    mpr_subscriber *sub = &dev->subscribers;
    mpr_time t;
    if (*sub)
        mpr_time_set(&t, MPR_NOW);
    while (*sub) {
        if ((*sub)->lease_exp < t.sec || !(*sub)->flags) {
            /* subscription expired, remove from subscriber list */
#ifdef DEBUG
            char *addr = lo_address_get_url((*sub)->addr);
            trace_dev(dev, "removing expired subscription from %s\n", addr);
            free(addr);
#endif
            mpr_subscriber temp = *sub;
            *sub = temp->next;
            FUNC_IF(lo_address_free, temp->addr);
            free(temp);
            continue;
        }
        if ((*sub)->flags & msg_type)
            lo_send_bundle_from((*sub)->addr, from, bundle);
        sub = &(*sub)->next;
    }
}

void mpr_local_dev_restart_registration(mpr_local_dev dev, int start_ordinal)
{
    dev->registered = 0;
    dev->ordinal_allocator.val = start_ordinal;
}

/*! Algorithm for checking collisions and allocating resources. */
static int check_collisions(mpr_net net, mpr_allocated resource)
{
    int i;
    double current_time, timediff;
    RETURN_ARG_UNLESS(!resource->locked, 0);
    current_time = mpr_get_current_time();
    timediff = current_time - resource->count_time;

    if (!resource->online) {
        if (timediff >= 5.0) {
            /* reprobe with the same value */
            resource->count_time = current_time;
            return 1;
        }
        return 0;
    }
    else if (timediff >= 2.0 && resource->collision_count < 2) {
        resource->locked = 1;
        return 2;
    }
    else if (timediff >= 0.5 && resource->collision_count > 1) {
        for (i = 0; i < 8; i++) {
            if (!resource->hints[i])
                break;
        }
        resource->val += i + (rand() % net->num_devs);

        /* Prepare for causing new resource collisions. */
        resource->collision_count = 0;
        resource->count_time = current_time;
        for (i = 0; i < 8; i++)
            resource->hints[i] = 0;

        /* Indicate that we need to re-probe the new value. */
        return 1;
    }
    return 0;
}

int mpr_local_dev_check_registration(mpr_local_dev dev)
{
    mpr_net net = mpr_graph_get_net(mpr_obj_get_graph(&dev->obj));
    if (dev->registered)
        return 1;

    /* If the ordinal has changed, re-probe the new name. */
    if (1 == check_collisions(net, &dev->ordinal_allocator))
        mpr_local_dev_probe_name(dev, net);
    else if (dev->ordinal_allocator.locked) {
        /* If we are ready to register the device, add the message handlers. */
        mpr_dev_on_registered(dev);

        /* Send registered msg. */
        mpr_net_send_name_registered(net, dev->name, -1, 0);

        mpr_net_add_dev_methods(net, dev);
        mpr_net_maybe_send_ping(net, 1);
        trace_dev(dev, "registered.\n");

        /* Send out any cached maps. */
        mpr_net_use_bus(net);
        mpr_dev_send_maps(dev, MPR_DIR_ANY, MSG_MAP);
        mpr_net_send(net);
        return 1;
    }
    return 0;
}

void mpr_local_dev_handler_logout(mpr_local_dev dev, mpr_dev remote, const char *prefix_str,
                                 int ordinal)
{
    mpr_link lnk;
    if (!dev->ordinal_allocator.locked)
        return;
    /* Check if we have any links to this device, if so remove them */
    if (remote && (lnk = mpr_dev_get_link_by_remote(dev, remote))) {
        /* TODO: release maps, call local handlers and inform subscribers */
        mpr_graph gph = mpr_obj_get_graph((mpr_obj)dev);
        mpr_net net = mpr_graph_get_net(gph);
        trace_dev(dev, "removing link to removed device '%s'.\n", mpr_dev_get_name(remote));
        mpr_rtr_remove_link(net->rtr, lnk);
        mpr_graph_remove_link(gph, lnk, MPR_OBJ_REM);
    }
    if (0 == strncmp(prefix_str, dev->name, dev->prefix_len)) {
        /* If device name matches and ordinal is within my block, free it */
        int diff = ordinal - dev->ordinal_allocator.val - 1;
        if (diff >= 0 && diff < 8)
            dev->ordinal_allocator.hints[diff] = 0;
    }
}

void mpr_local_dev_copy_net_servers(mpr_local_dev dev, lo_server *servers)
{
    memcpy(dev->servers + 2, servers, sizeof(lo_server) * 2);
}

void mpr_dev_set_num_maps(mpr_dev dev, int num_maps_in, int num_maps_out)
{
    dev->num_maps_in = num_maps_in;
    dev->num_maps_out = num_maps_out;
}

void mpr_local_dev_add_sig(mpr_local_dev dev, mpr_local_sig sig, mpr_dir dir)
{
    if (dir == MPR_DIR_IN)
        ++dev->num_inputs;
    else
        ++dev->num_outputs;

    mpr_obj_increment_version((mpr_obj)dev);
    mpr_dev_add_sig_methods(dev, sig);
    if (dev->registered) {
        /* Notify subscribers */
        mpr_graph graph = mpr_obj_get_graph((mpr_obj)dev);
        mpr_net_use_subscribers(mpr_graph_get_net(graph), dev,
                                ((dir == MPR_DIR_IN) ? MPR_SIG_IN : MPR_SIG_OUT));
        mpr_sig_send_state((mpr_sig)sig, MSG_SIG);
    }
}
