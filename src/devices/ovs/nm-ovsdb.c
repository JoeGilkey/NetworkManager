/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-ovsdb.h"

#include <gmodule.h>
#include <gio/gunixsocketaddress.h>

#include "nm-glib-aux/nm-jansson.h"
#include "nm-core-utils.h"
#include "nm-core-internal.h"
#include "devices/nm-device.h"

/*****************************************************************************/

#if JANSSON_VERSION_HEX < 0x020400
    #warning "requires at least libjansson 2.4"
#endif

typedef struct {
    char *     name;
    char *     connection_uuid;
    GPtrArray *interfaces; /* interface uuids */
} OpenvswitchPort;

typedef struct {
    char *     bridge_uuid;
    char *     name;
    char *     connection_uuid;
    GPtrArray *ports; /* port uuids */
} OpenvswitchBridge;

typedef struct {
    char *name;
    char *type;
    char *connection_uuid;
} OpenvswitchInterface;

/*****************************************************************************/

enum { DEVICE_ADDED, DEVICE_REMOVED, INTERFACE_FAILED, LAST_SIGNAL };

static guint signals[LAST_SIGNAL] = {0};

typedef struct {
    GSocketClient *    client;
    GSocketConnection *conn;
    GCancellable *     cancellable;
    char               buf[4096]; /* Input buffer */
    size_t             bufp;      /* Last decoded byte in the input buffer. */
    GString *          input;     /* JSON stream waiting for decoding. */
    GString *          output;    /* JSON stream to be sent. */
    guint64            call_id_counter;
    GArray *           calls;      /* Method calls waiting for a response. */
    GHashTable *       interfaces; /* interface uuid => OpenvswitchInterface */
    GHashTable *       ports;      /* port uuid => OpenvswitchPort */
    GHashTable *       bridges;    /* bridge uuid => OpenvswitchBridge */
    char *             db_uuid;
    guint              num_failures;
} NMOvsdbPrivate;

struct _NMOvsdb {
    GObject        parent;
    NMOvsdbPrivate _priv;
};

struct _NMOvsdbClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMOvsdb, nm_ovsdb, G_TYPE_OBJECT)

#define NM_OVSDB_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMOvsdb, NM_IS_OVSDB)

#define _NMLOG_DOMAIN      LOGD_DEVICE
#define _NMLOG(level, ...) __NMLOG_DEFAULT(level, _NMLOG_DOMAIN, "ovsdb", __VA_ARGS__)

NM_DEFINE_SINGLETON_GETTER(NMOvsdb, nm_ovsdb_get, NM_TYPE_OVSDB);

/*****************************************************************************/

static void ovsdb_try_connect(NMOvsdb *self);
static void ovsdb_disconnect(NMOvsdb *self, gboolean retry, gboolean is_disposing);
static void ovsdb_read(NMOvsdb *self);
static void ovsdb_write(NMOvsdb *self);
static void ovsdb_next_command(NMOvsdb *self);

/*****************************************************************************/

/* ovsdb command abstraction. */

typedef void (*OvsdbMethodCallback)(NMOvsdb *self,
                                    json_t * response,
                                    GError * error,
                                    gpointer user_data);

typedef enum {
    OVSDB_MONITOR,
    OVSDB_ADD_INTERFACE,
    OVSDB_DEL_INTERFACE,
    OVSDB_SET_INTERFACE_MTU,
} OvsdbCommand;

#define CALL_ID_UNSPEC G_MAXUINT64

typedef struct {
    guint64             call_id;
    OvsdbCommand        command;
    OvsdbMethodCallback callback;
    gpointer            user_data;
    union {
        struct {
            char *  ifname;
            guint32 mtu;
        };
        struct {
            NMConnection *bridge;
            NMConnection *port;
            NMConnection *interface;
            NMDevice *    bridge_device;
            NMDevice *    interface_device;
        };
    };
} OvsdbMethodCall;

#define OVSDB_MAX_FAILURES 3

static void
_LOGT_call_do(const char *comment, OvsdbMethodCall *call, json_t *msg)
{
    gs_free char *str = NULL;

    if (msg)
        str = json_dumps(msg, 0);

    switch (call->command) {
    case OVSDB_MONITOR:
        _LOGT("%s: monitor%s%s", comment, msg ? ": " : "", msg ? str : "");
        break;
    case OVSDB_ADD_INTERFACE:
        _LOGT("%s: add-iface bridge=%s port=%s interface=%s%s%s",
              comment,
              nm_connection_get_interface_name(call->bridge),
              nm_connection_get_interface_name(call->port),
              nm_connection_get_interface_name(call->interface),
              msg ? ": " : "",
              msg ? str : "");
        break;
    case OVSDB_DEL_INTERFACE:
        _LOGT("%s: del-iface interface=%s%s%s",
              comment,
              call->ifname,
              msg ? ": " : "",
              msg ? str : "");
        break;
    case OVSDB_SET_INTERFACE_MTU:
        _LOGT("%s: set-iface-mtu interface=%s%s%s mtu=%u",
              comment,
              call->ifname,
              msg ? ": " : "",
              msg ? str : "",
              call->mtu);
        break;
    }
}

#define _LOGT_call(comment, call, message)               \
    G_STMT_START                                         \
    {                                                    \
        if (_LOGT_ENABLED())                             \
            _LOGT_call_do((comment), (call), (message)); \
    }                                                    \
    G_STMT_END

/**
 * ovsdb_call_method:
 *
 * Queues the ovsdb command. Eventually fires the command right away if
 * there's no command pending completion.
 */
static void
ovsdb_call_method(NMOvsdb *           self,
                  OvsdbCommand        command,
                  const char *        ifname,
                  NMConnection *      bridge,
                  NMConnection *      port,
                  NMConnection *      interface,
                  NMDevice *          bridge_device,
                  NMDevice *          interface_device,
                  guint32             mtu,
                  OvsdbMethodCallback callback,
                  gpointer            user_data,
                  gboolean            add_first)
{
    NMOvsdbPrivate * priv = NM_OVSDB_GET_PRIVATE(self);
    OvsdbMethodCall *call;

    /* Ensure we're not unsynchronized before we queue the method call. */
    ovsdb_try_connect(self);

    if (add_first) {
        g_array_prepend_val(priv->calls, (OvsdbMethodCall){});
        call = &g_array_index(priv->calls, OvsdbMethodCall, 0);
    } else {
        g_array_set_size(priv->calls, priv->calls->len + 1);
        call = &g_array_index(priv->calls, OvsdbMethodCall, priv->calls->len - 1);
    }
    call->call_id   = CALL_ID_UNSPEC;
    call->command   = command;
    call->callback  = callback;
    call->user_data = user_data;

    switch (call->command) {
    case OVSDB_MONITOR:
        break;
    case OVSDB_ADD_INTERFACE:
        /* FIXME(applied-connection-immutable): we should not modify the applied
         *   connection, consequently there is no need to clone the connections. */
        call->bridge           = nm_simple_connection_new_clone(bridge);
        call->port             = nm_simple_connection_new_clone(port);
        call->interface        = nm_simple_connection_new_clone(interface);
        call->bridge_device    = g_object_ref(bridge_device);
        call->interface_device = g_object_ref(interface_device);
        break;
    case OVSDB_DEL_INTERFACE:
        call->ifname = g_strdup(ifname);
        break;
    case OVSDB_SET_INTERFACE_MTU:
        call->ifname = g_strdup(ifname);
        call->mtu    = mtu;
        break;
    }

    _LOGT_call("enqueue", call, NULL);

    ovsdb_next_command(self);
}

/*****************************************************************************/

/* Create and process the JSON-RPC messages from ovsdb. */

/**
 * _expect_ovs_bridges:
 *
 * Return a command that will fail the transaction if the actual set of
 * bridges doesn't match @bridges. This is a way of detecting race conditions
 * with other ovsdb clients that might be adding or removing bridges
 * at the same time.
 */
static void
_expect_ovs_bridges(json_t *params, const char *db_uuid, json_t *bridges)
{
    json_array_append_new(
        params,
        json_pack("{s:s, s:s, s:i, s:[s], s:s, s:[{s:[s, O]}], s:[[s, s, [s, s]]]}",
                  "op",
                  "wait",
                  "table",
                  "Open_vSwitch",
                  "timeout",
                  0,
                  "columns",
                  "bridges",
                  "until",
                  "==",
                  "rows",
                  "bridges",
                  "set",
                  bridges,
                  "where",
                  "_uuid",
                  "==",
                  "uuid",
                  db_uuid));
}

/**
 * _set_ovs_bridges:
 *
 * Return a command that will update the list of bridges in @db_uuid
 * database to @new_bridges.
 */
static void
_set_ovs_bridges(json_t *params, const char *db_uuid, json_t *new_bridges)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:{s:[s, O]}, s:[[s, s, [s, s]]]}",
                                    "op",
                                    "update",
                                    "table",
                                    "Open_vSwitch",
                                    "row",
                                    "bridges",
                                    "set",
                                    new_bridges,
                                    "where",
                                    "_uuid",
                                    "==",
                                    "uuid",
                                    db_uuid));
}

/**
 * _expect_bridge_ports:
 *
 * Return a command that will fail the transaction if the actual set of
 * ports in bridge @ifname doesn't match @ports. This is a way of detecting
 * race conditions with other ovsdb clients that might be adding or removing
 * bridge ports at the same time.
 */
static void
_expect_bridge_ports(json_t *params, const char *ifname, json_t *ports)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:i, s:[s], s:s, s:[{s:[s, O]}], s:[[s, s, s]]}",
                                    "op",
                                    "wait",
                                    "table",
                                    "Bridge",
                                    "timeout",
                                    0,
                                    "columns",
                                    "ports",
                                    "until",
                                    "==",
                                    "rows",
                                    "ports",
                                    "set",
                                    ports,
                                    "where",
                                    "name",
                                    "==",
                                    ifname));
}

/**
 * _set_bridge_ports:
 *
 * Return a command that will update the list of ports of bridge
 * @ifname to @new_ports.
 */
static void
_set_bridge_ports(json_t *params, const char *ifname, json_t *new_ports)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:{s:[s, O]}, s:[[s, s, s]]}",
                                    "op",
                                    "update",
                                    "table",
                                    "Bridge",
                                    "row",
                                    "ports",
                                    "set",
                                    new_ports,
                                    "where",
                                    "name",
                                    "==",
                                    ifname));
}

static void
_set_bridge_mac(json_t *params, const char *ifname, const char *mac)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:{s:[s, [[s, s]]]}, s:[[s, s, s]]}",
                                    "op",
                                    "update",
                                    "table",
                                    "Bridge",
                                    "row",
                                    "other_config",
                                    "map",
                                    "hwaddr",
                                    mac,
                                    "where",
                                    "name",
                                    "==",
                                    ifname));
}

/**
 * _expect_port_interfaces:
 *
 * Return a command that will fail the transaction if the actual set of
 * interfaces in port @ifname doesn't match @interfaces. This is a way of
 * detecting race conditions with other ovsdb clients that might be adding
 * or removing port interfaces at the same time.
 */
static void
_expect_port_interfaces(json_t *params, const char *ifname, json_t *interfaces)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:i, s:[s], s:s, s:[{s:[s, O]}], s:[[s, s, s]]}",
                                    "op",
                                    "wait",
                                    "table",
                                    "Port",
                                    "timeout",
                                    0,
                                    "columns",
                                    "interfaces",
                                    "until",
                                    "==",
                                    "rows",
                                    "interfaces",
                                    "set",
                                    interfaces,
                                    "where",
                                    "name",
                                    "==",
                                    ifname));
}

/**
 * _set_port_interfaces:
 *
 * Return a command that will update the list of interfaces of port @ifname
 * to @new_interfaces.
 */
static void
_set_port_interfaces(json_t *params, const char *ifname, json_t *new_interfaces)
{
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:{s:[s, O]}, s:[[s, s, s]]}",
                                    "op",
                                    "update",
                                    "table",
                                    "Port",
                                    "row",
                                    "interfaces",
                                    "set",
                                    new_interfaces,
                                    "where",
                                    "name",
                                    "==",
                                    ifname));
}

/**
 * _insert_interface:
 *
 * Returns an commands that adds new interface from a given connection.
 */
static void
_insert_interface(json_t *      params,
                  NMConnection *interface,
                  NMDevice *    interface_device,
                  const char *  cloned_mac)
{
    const char *           type = NULL;
    NMSettingOvsInterface *s_ovs_iface;
    NMSettingOvsDpdk *     s_ovs_dpdk;
    NMSettingOvsPatch *    s_ovs_patch;
    json_t *               options = json_array();
    json_t *               row;
    guint32                mtu = 0;

    s_ovs_iface = nm_connection_get_setting_ovs_interface(interface);
    if (s_ovs_iface)
        type = nm_setting_ovs_interface_get_interface_type(s_ovs_iface);

    if (nm_streq0(type, "internal")) {
        NMSettingWired *s_wired;

        s_wired = _nm_connection_get_setting(interface, NM_TYPE_SETTING_WIRED);
        if (s_wired)
            mtu = nm_setting_wired_get_mtu(s_wired);
    }

    json_array_append_new(options, json_string("map"));

    s_ovs_dpdk =
        (NMSettingOvsDpdk *) nm_connection_get_setting(interface, NM_TYPE_SETTING_OVS_DPDK);
    if (!s_ovs_dpdk)
        s_ovs_patch = nm_connection_get_setting_ovs_patch(interface);

    if (s_ovs_dpdk) {
        json_array_append_new(
            options,
            json_pack("[[s, s]]", "dpdk-devargs", nm_setting_ovs_dpdk_get_devargs(s_ovs_dpdk)));
    } else if (s_ovs_patch) {
        json_array_append_new(
            options,
            json_pack("[[s, s]]", "peer", nm_setting_ovs_patch_get_peer(s_ovs_patch)));
    } else {
        json_array_append_new(options, json_array());
    }

    row = json_pack("{s:s, s:s, s:o, s:[s, [[s, s]]]}",
                    "name",
                    nm_connection_get_interface_name(interface),
                    "type",
                    type ?: "",
                    "options",
                    options,
                    "external_ids",
                    "map",
                    "NM.connection.uuid",
                    nm_connection_get_uuid(interface));

    if (cloned_mac)
        json_object_set_new(row, "mac", json_string(cloned_mac));

    if (mtu != 0)
        json_object_set_new(row, "mtu_request", json_integer(mtu));

    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:o, s:s}",
                                    "op",
                                    "insert",
                                    "table",
                                    "Interface",
                                    "row",
                                    row,
                                    "uuid-name",
                                    "rowInterface"));
}

/**
 * _insert_port:
 *
 * Returns an commands that adds new port from a given connection.
 */
static void
_insert_port(json_t *params, NMConnection *port, json_t *new_interfaces)
{
    NMSettingOvsPort *s_ovs_port;
    const char *      vlan_mode      = NULL;
    guint             tag            = 0;
    const char *      lacp           = NULL;
    const char *      bond_mode      = NULL;
    guint             bond_updelay   = 0;
    guint             bond_downdelay = 0;
    json_t *          row;

    s_ovs_port = nm_connection_get_setting_ovs_port(port);

    row = json_object();

    if (s_ovs_port) {
        vlan_mode      = nm_setting_ovs_port_get_vlan_mode(s_ovs_port);
        tag            = nm_setting_ovs_port_get_tag(s_ovs_port);
        lacp           = nm_setting_ovs_port_get_lacp(s_ovs_port);
        bond_mode      = nm_setting_ovs_port_get_bond_mode(s_ovs_port);
        bond_updelay   = nm_setting_ovs_port_get_bond_updelay(s_ovs_port);
        bond_downdelay = nm_setting_ovs_port_get_bond_downdelay(s_ovs_port);
    }

    if (vlan_mode)
        json_object_set_new(row, "vlan_mode", json_string(vlan_mode));
    if (tag)
        json_object_set_new(row, "tag", json_integer(tag));
    if (lacp)
        json_object_set_new(row, "lacp", json_string(lacp));
    if (bond_mode)
        json_object_set_new(row, "bond_mode", json_string(bond_mode));
    if (bond_updelay)
        json_object_set_new(row, "bond_updelay", json_integer(bond_updelay));
    if (bond_downdelay)
        json_object_set_new(row, "bond_downdelay", json_integer(bond_downdelay));

    json_object_set_new(row, "name", json_string(nm_connection_get_interface_name(port)));
    json_object_set_new(row, "interfaces", json_pack("[s, O]", "set", new_interfaces));
    json_object_set_new(
        row,
        "external_ids",
        json_pack("[s, [[s, s]]]", "map", "NM.connection.uuid", nm_connection_get_uuid(port)));

    /* Create a new one. */
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:o, s:s}",
                                    "op",
                                    "insert",
                                    "table",
                                    "Port",
                                    "row",
                                    row,
                                    "uuid-name",
                                    "rowPort"));
}

/**
 * _insert_bridge:
 *
 * Returns an commands that adds new bridge from a given connection.
 */
static void
_insert_bridge(json_t *      params,
               NMConnection *bridge,
               NMDevice *    bridge_device,
               json_t *      new_ports,
               const char *  cloned_mac)
{
    NMSettingOvsBridge *s_ovs_bridge;
    const char *        fail_mode             = NULL;
    gboolean            mcast_snooping_enable = FALSE;
    gboolean            rstp_enable           = FALSE;
    gboolean            stp_enable            = FALSE;
    const char *        datapath_type         = NULL;
    json_t *            row;

    s_ovs_bridge = nm_connection_get_setting_ovs_bridge(bridge);

    row = json_object();

    if (s_ovs_bridge) {
        fail_mode             = nm_setting_ovs_bridge_get_fail_mode(s_ovs_bridge);
        mcast_snooping_enable = nm_setting_ovs_bridge_get_mcast_snooping_enable(s_ovs_bridge);
        rstp_enable           = nm_setting_ovs_bridge_get_rstp_enable(s_ovs_bridge);
        stp_enable            = nm_setting_ovs_bridge_get_stp_enable(s_ovs_bridge);
        datapath_type         = nm_setting_ovs_bridge_get_datapath_type(s_ovs_bridge);
    }

    if (fail_mode)
        json_object_set_new(row, "fail_mode", json_string(fail_mode));
    if (mcast_snooping_enable)
        json_object_set_new(row, "mcast_snooping_enable", json_boolean(mcast_snooping_enable));
    if (rstp_enable)
        json_object_set_new(row, "rstp_enable", json_boolean(rstp_enable));
    if (stp_enable)
        json_object_set_new(row, "stp_enable", json_boolean(stp_enable));
    if (datapath_type)
        json_object_set_new(row, "datapath_type", json_string(datapath_type));

    json_object_set_new(row, "name", json_string(nm_connection_get_interface_name(bridge)));
    json_object_set_new(row, "ports", json_pack("[s, O]", "set", new_ports));
    json_object_set_new(
        row,
        "external_ids",
        json_pack("[s, [[s, s]]]", "map", "NM.connection.uuid", nm_connection_get_uuid(bridge)));

    if (cloned_mac) {
        json_object_set_new(row,
                            "other_config",
                            json_pack("[s, [[s, s]]]", "map", "hwaddr", cloned_mac));
    }

    /* Create a new one. */
    json_array_append_new(params,
                          json_pack("{s:s, s:s, s:o, s:s}",
                                    "op",
                                    "insert",
                                    "table",
                                    "Bridge",
                                    "row",
                                    row,
                                    "uuid-name",
                                    "rowBridge"));
}

/**
 * _inc_next_cfg:
 *
 * Returns an mutate command that bumps next_cfg upon successful completion
 * of the transaction it is in.
 */
static json_t *
_inc_next_cfg(const char *db_uuid)
{
    return json_pack("{s:s, s:s, s:[[s, s, i]], s:[[s, s, [s, s]]]}",
                     "op",
                     "mutate",
                     "table",
                     "Open_vSwitch",
                     "mutations",
                     "next_cfg",
                     "+=",
                     1,
                     "where",
                     "_uuid",
                     "==",
                     "uuid",
                     db_uuid);
}

/**
 * _add_interface:
 *
 * Adds an interface as specified by @interface connection, optionally creating
 * a parent @port and @bridge if needed.
 */
static void
_add_interface(NMOvsdb *     self,
               json_t *      params,
               NMConnection *bridge,
               NMConnection *port,
               NMConnection *interface,
               NMDevice *    bridge_device,
               NMDevice *    interface_device)
{
    NMOvsdbPrivate *      priv = NM_OVSDB_GET_PRIVATE(self);
    GHashTableIter        iter;
    const char *          port_uuid;
    const char *          interface_uuid;
    const char *          bridge_name;
    const char *          port_name;
    const char *          interface_name;
    OpenvswitchBridge *   ovs_bridge           = NULL;
    OpenvswitchPort *     ovs_port             = NULL;
    OpenvswitchInterface *ovs_interface        = NULL;
    nm_auto_decref_json json_t *bridges        = NULL;
    nm_auto_decref_json json_t *new_bridges    = NULL;
    nm_auto_decref_json json_t *ports          = NULL;
    nm_auto_decref_json json_t *new_ports      = NULL;
    nm_auto_decref_json json_t *interfaces     = NULL;
    nm_auto_decref_json json_t *new_interfaces = NULL;
    gboolean                    has_interface  = FALSE;
    gboolean                    interface_is_internal;
    gs_free char *              bridge_cloned_mac    = NULL;
    gs_free char *              interface_cloned_mac = NULL;
    GError *                    error                = NULL;
    int                         pi;
    int                         ii;

    bridges        = json_array();
    ports          = json_array();
    interfaces     = json_array();
    new_bridges    = json_array();
    new_ports      = json_array();
    new_interfaces = json_array();

    bridge_name           = nm_connection_get_interface_name(bridge);
    port_name             = nm_connection_get_interface_name(port);
    interface_name        = nm_connection_get_interface_name(interface);
    interface_is_internal = nm_streq0(bridge_name, interface_name);

    /* Determine cloned MAC addresses */
    if (!nm_device_hw_addr_get_cloned(bridge_device,
                                      bridge,
                                      FALSE,
                                      &bridge_cloned_mac,
                                      NULL,
                                      &error)) {
        _LOGW("Cannot determine cloned mac for OVS %s '%s': %s",
              "bridge",
              bridge_name,
              error->message);
        g_clear_error(&error);
    }

    if (!nm_device_hw_addr_get_cloned(interface_device,
                                      interface,
                                      FALSE,
                                      &interface_cloned_mac,
                                      NULL,
                                      &error)) {
        _LOGW("Cannot determine cloned mac for OVS %s '%s': %s",
              "interface",
              interface_name,
              error->message);
        g_clear_error(&error);
    }

    if (interface_is_internal && !bridge_cloned_mac && interface_cloned_mac) {
        _LOGT("'%s' is a local ovs-interface, the MAC will be set on ovs-bridge '%s'",
              interface_name,
              bridge_name);
        bridge_cloned_mac = g_steal_pointer(&interface_cloned_mac);
    }

    g_hash_table_iter_init(&iter, priv->bridges);
    while (g_hash_table_iter_next(&iter, (gpointer) &ovs_bridge, NULL)) {
        json_array_append_new(bridges, json_pack("[s, s]", "uuid", ovs_bridge->bridge_uuid));

        if (!nm_streq0(ovs_bridge->name, bridge_name)
            || !nm_streq0(ovs_bridge->connection_uuid, nm_connection_get_uuid(bridge)))
            continue;

        for (pi = 0; pi < ovs_bridge->ports->len; pi++) {
            port_uuid = g_ptr_array_index(ovs_bridge->ports, pi);
            ovs_port  = g_hash_table_lookup(priv->ports, port_uuid);

            json_array_append_new(ports, json_pack("[s, s]", "uuid", port_uuid));

            if (!ovs_port) {
                /* This would be a violation of ovsdb's reference integrity (a bug). */
                _LOGW("Unknown port '%s' in bridge '%s'", port_uuid, ovs_bridge->bridge_uuid);
                continue;
            }

            if (!nm_streq(ovs_port->name, port_name)
                || !nm_streq0(ovs_port->connection_uuid, nm_connection_get_uuid(port)))
                continue;

            for (ii = 0; ii < ovs_port->interfaces->len; ii++) {
                interface_uuid = g_ptr_array_index(ovs_port->interfaces, ii);
                ovs_interface  = g_hash_table_lookup(priv->interfaces, interface_uuid);

                json_array_append_new(interfaces, json_pack("[s, s]", "uuid", interface_uuid));

                if (!ovs_interface) {
                    /* This would be a violation of ovsdb's reference integrity (a bug). */
                    _LOGW("Unknown interface '%s' in port '%s'", interface_uuid, port_uuid);
                    continue;
                }
                if (nm_streq(ovs_interface->name, interface_name)
                    && nm_streq0(ovs_interface->connection_uuid, nm_connection_get_uuid(interface)))
                    has_interface = TRUE;
            }

            break;
        }

        break;
    }

    json_array_extend(new_bridges, bridges);
    json_array_extend(new_ports, ports);
    json_array_extend(new_interfaces, interfaces);

    if (json_array_size(interfaces) == 0) {
        /* Need to create a port. */
        if (json_array_size(ports) == 0) {
            /* Need to create a bridge. */
            _expect_ovs_bridges(params, priv->db_uuid, bridges);
            json_array_append_new(new_bridges, json_pack("[s, s]", "named-uuid", "rowBridge"));
            _set_ovs_bridges(params, priv->db_uuid, new_bridges);
            _insert_bridge(params, bridge, bridge_device, new_ports, bridge_cloned_mac);
        } else {
            /* Bridge already exists. */
            g_return_if_fail(ovs_bridge);
            _expect_bridge_ports(params, ovs_bridge->name, ports);
            _set_bridge_ports(params, bridge_name, new_ports);
            if (bridge_cloned_mac && interface_is_internal)
                _set_bridge_mac(params, bridge_name, bridge_cloned_mac);
        }

        json_array_append_new(new_ports, json_pack("[s, s]", "named-uuid", "rowPort"));
        _insert_port(params, port, new_interfaces);
    } else {
        /* Port already exists */
        g_return_if_fail(ovs_port);
        _expect_port_interfaces(params, ovs_port->name, interfaces);
        _set_port_interfaces(params, port_name, new_interfaces);
    }

    if (!has_interface) {
        _insert_interface(params, interface, interface_device, interface_cloned_mac);
        json_array_append_new(new_interfaces, json_pack("[s, s]", "named-uuid", "rowInterface"));
    }
}

/**
 * _delete_interface:
 *
 * Removes an interface of @ifname name, collecting empty ports and bridge
 * if last item is removed from them.
 */
static void
_delete_interface(NMOvsdb *self, json_t *params, const char *ifname)
{
    NMOvsdbPrivate *      priv = NM_OVSDB_GET_PRIVATE(self);
    GHashTableIter        iter;
    char *                port_uuid;
    char *                interface_uuid;
    OpenvswitchBridge *   ovs_bridge;
    OpenvswitchPort *     ovs_port;
    OpenvswitchInterface *ovs_interface;
    nm_auto_decref_json json_t *bridges     = NULL;
    nm_auto_decref_json json_t *new_bridges = NULL;
    gboolean                    bridges_changed;
    gboolean                    ports_changed;
    gboolean                    interfaces_changed;
    int                         pi;
    int                         ii;

    bridges         = json_array();
    new_bridges     = json_array();
    bridges_changed = FALSE;

    g_hash_table_iter_init(&iter, priv->bridges);
    while (g_hash_table_iter_next(&iter, (gpointer) &ovs_bridge, NULL)) {
        nm_auto_decref_json json_t *ports     = NULL;
        nm_auto_decref_json json_t *new_ports = NULL;

        ports         = json_array();
        new_ports     = json_array();
        ports_changed = FALSE;

        json_array_append_new(bridges, json_pack("[s,s]", "uuid", ovs_bridge->bridge_uuid));

        for (pi = 0; pi < ovs_bridge->ports->len; pi++) {
            nm_auto_decref_json json_t *interfaces     = NULL;
            nm_auto_decref_json json_t *new_interfaces = NULL;

            interfaces     = json_array();
            new_interfaces = json_array();
            port_uuid      = g_ptr_array_index(ovs_bridge->ports, pi);
            ovs_port       = g_hash_table_lookup(priv->ports, port_uuid);

            json_array_append_new(ports, json_pack("[s,s]", "uuid", port_uuid));

            interfaces_changed = FALSE;

            if (!ovs_port) {
                /* This would be a violation of ovsdb's reference integrity (a bug). */
                _LOGW("Unknown port '%s' in bridge '%s'", port_uuid, ovs_bridge->bridge_uuid);
                continue;
            }

            for (ii = 0; ii < ovs_port->interfaces->len; ii++) {
                interface_uuid = g_ptr_array_index(ovs_port->interfaces, ii);
                ovs_interface  = g_hash_table_lookup(priv->interfaces, interface_uuid);

                json_array_append_new(interfaces, json_pack("[s,s]", "uuid", interface_uuid));

                if (ovs_interface) {
                    if (nm_streq(ovs_interface->name, ifname)) {
                        /* skip the interface */
                        interfaces_changed = TRUE;
                        continue;
                    }
                } else {
                    /* This would be a violation of ovsdb's reference integrity (a bug). */
                    _LOGW("Unknown interface '%s' in port '%s'", interface_uuid, port_uuid);
                }

                json_array_append_new(new_interfaces, json_pack("[s,s]", "uuid", interface_uuid));
            }

            if (json_array_size(new_interfaces) == 0) {
                ports_changed = TRUE;
            } else {
                if (interfaces_changed) {
                    _expect_port_interfaces(params, ovs_port->name, interfaces);
                    _set_port_interfaces(params, ovs_port->name, new_interfaces);
                }
                json_array_append_new(new_ports, json_pack("[s,s]", "uuid", port_uuid));
            }
        }

        if (json_array_size(new_ports) == 0) {
            bridges_changed = TRUE;
        } else {
            if (ports_changed) {
                _expect_bridge_ports(params, ovs_bridge->name, ports);
                _set_bridge_ports(params, ovs_bridge->name, new_ports);
            }
            json_array_append_new(new_bridges, json_pack("[s,s]", "uuid", ovs_bridge->bridge_uuid));
        }
    }

    if (bridges_changed) {
        _expect_ovs_bridges(params, priv->db_uuid, bridges);
        _set_ovs_bridges(params, priv->db_uuid, new_bridges);
    }
}

/**
 * ovsdb_next_command:
 *
 * Translates a higher level operation (add/remove bridge/port) to a RFC 7047
 * command serialized into JSON ands sends it over to the database.

 * Only called when no command is waiting for a response, since the serialized
 * command might depend on result of a previous one (add and remove need to
 * include an up to date bridge list in their transactions to rule out races).
 */
static void
ovsdb_next_command(NMOvsdb *self)
{
    NMOvsdbPrivate *    priv = NM_OVSDB_GET_PRIVATE(self);
    OvsdbMethodCall *   call = NULL;
    char *              cmd;
    nm_auto_decref_json json_t *msg = NULL;
    json_t *                    params;

    if (!priv->conn)
        return;
    if (!priv->calls->len)
        return;
    call = &g_array_index(priv->calls, OvsdbMethodCall, 0);
    if (call->call_id != CALL_ID_UNSPEC)
        return;

    call->call_id = ++priv->call_id_counter;

    switch (call->command) {
    case OVSDB_MONITOR:
        msg = json_pack("{s:I, s:s, s:[s, n, {"
                        "  s:[{s:[s, s, s]}],"
                        "  s:[{s:[s, s, s]}],"
                        "  s:[{s:[s, s, s, s]}],"
                        "  s:[{s:[]}]"
                        "}]}",
                        "id",
                        (json_int_t) call->call_id,
                        "method",
                        "monitor",
                        "params",
                        "Open_vSwitch",
                        "Bridge",
                        "columns",
                        "name",
                        "ports",
                        "external_ids",
                        "Port",
                        "columns",
                        "name",
                        "interfaces",
                        "external_ids",
                        "Interface",
                        "columns",
                        "name",
                        "type",
                        "external_ids",
                        "error",
                        "Open_vSwitch",
                        "columns");
        break;
    case OVSDB_ADD_INTERFACE:
        params = json_array();
        json_array_append_new(params, json_string("Open_vSwitch"));
        json_array_append_new(params, _inc_next_cfg(priv->db_uuid));

        _add_interface(self,
                       params,
                       call->bridge,
                       call->port,
                       call->interface,
                       call->bridge_device,
                       call->interface_device);

        msg = json_pack("{s:I, s:s, s:o}",
                        "id",
                        (json_int_t) call->call_id,
                        "method",
                        "transact",
                        "params",
                        params);
        break;
    case OVSDB_DEL_INTERFACE:
        params = json_array();
        json_array_append_new(params, json_string("Open_vSwitch"));
        json_array_append_new(params, _inc_next_cfg(priv->db_uuid));

        _delete_interface(self, params, call->ifname);

        msg = json_pack("{s:I, s:s, s:o}",
                        "id",
                        (json_int_t) call->call_id,
                        "method",
                        "transact",
                        "params",
                        params);
        break;
    case OVSDB_SET_INTERFACE_MTU:
        params = json_array();
        json_array_append_new(params, json_string("Open_vSwitch"));
        json_array_append_new(params, _inc_next_cfg(priv->db_uuid));

        json_array_append_new(params,
                              json_pack("{s:s, s:s, s:{s: I}, s:[[s, s, s]]}",
                                        "op",
                                        "update",
                                        "table",
                                        "Interface",
                                        "row",
                                        "mtu_request",
                                        (json_int_t) call->mtu,
                                        "where",
                                        "name",
                                        "==",
                                        call->ifname));

        msg = json_pack("{s:I, s:s, s:o}",
                        "id",
                        (json_int_t) call->call_id,
                        "method",
                        "transact",
                        "params",
                        params);
        break;
    }

    g_return_if_fail(msg);
    _LOGT_call("send", call, msg);
    cmd = json_dumps(msg, 0);

    g_string_append(priv->output, cmd);
    free(cmd);

    ovsdb_write(self);
}

/**
 * _uuids_to_array:
 *
 * This tidies up the somewhat non-straightforward way ovsdb represents an array
 * of UUID elements. The single element is a tuple (called <atom> in RFC7047),
 *
 *   [ "uuid", "aa095ffb-e1f1-0fc4-8038-82c1ea7e4797" ]
 *
 * while the list of multiple UUIDs are turned into a set of such tuples ("atoms"):
 *
 *   [ "set", [ [ "uuid", "aa095ffb-e1f1-0fc4-8038-82c1ea7e4797" ],
 *              [ "uuid", "185c93f6-0b39-424e-8587-77d074aa7ce0" ], ... ] ]
 */
static void
_uuids_to_array(GPtrArray *array, const json_t *items)
{
    const char *key;
    json_t *    value;
    size_t      index = 0;
    json_t *    set_value;
    size_t      set_index;

    while (index < json_array_size(items)) {
        key = json_string_value(json_array_get(items, index));
        index++;
        value = json_array_get(items, index);
        index++;

        if (!value)
            return;

        if (nm_streq0(key, "uuid") && json_is_string(value)) {
            g_ptr_array_add(array, g_strdup(json_string_value(value)));
        } else if (nm_streq0(key, "set") && json_is_array(value)) {
            json_array_foreach (value, set_index, set_value) {
                _uuids_to_array(array, set_value);
            }
        }
    }
}

static char *
_connection_uuid_from_external_ids(json_t *external_ids)
{
    json_t *value;
    size_t  index;

    if (!nm_streq0("map", json_string_value(json_array_get(external_ids, 0))))
        return NULL;

    json_array_foreach (json_array_get(external_ids, 1), index, value) {
        if (nm_streq0("NM.connection.uuid", json_string_value(json_array_get(value, 0))))
            return g_strdup(json_string_value(json_array_get(value, 1)));
    }

    return NULL;
}

/**
 * ovsdb_got_update:
 *
 * Called when we've got an "update" method call (we asked for it with the monitor
 * command). We use it to maintain a consistent view of bridge list regardless of
 * whether the changes are done by us or externally.
 */
static void
ovsdb_got_update(NMOvsdb *self, json_t *msg)
{
    NMOvsdbPrivate *priv      = NM_OVSDB_GET_PRIVATE(self);
    json_t *        ovs       = NULL;
    json_t *        bridge    = NULL;
    json_t *        port      = NULL;
    json_t *        interface = NULL;
    json_t *        items;
    json_t *        external_ids;
    json_error_t    json_error = {
        0,
    };
    void *                iter;
    const char *          name;
    const char *          key;
    const char *          type;
    json_t *              value;
    OpenvswitchBridge *   ovs_bridge;
    OpenvswitchPort *     ovs_port;
    OpenvswitchInterface *ovs_interface;

    if (json_unpack_ex(msg,
                       &json_error,
                       0,
                       "{s?:o, s?:o, s?:o, s?:o}",
                       "Open_vSwitch",
                       &ovs,
                       "Bridge",
                       &bridge,
                       "Port",
                       &port,
                       "Interface",
                       &interface)
        == -1) {
        /* This doesn't really have to be an error; the key might
         * be missing if there really are no bridges present. */
        _LOGD("Bad update: %s", json_error.text);
    }

    if (ovs) {
        const char *s;

        iter = json_object_iter(ovs);
        s    = json_object_iter_key(iter);
        if (s)
            nm_utils_strdup_reset(&priv->db_uuid, s);
    }

    /* Interfaces */
    json_object_foreach (interface, key, value) {
        json_t * error = NULL;
        gboolean old   = FALSE;
        gboolean new   = FALSE;

        if (json_unpack(value, "{s:{}}", "old") == 0)
            old = TRUE;

        if (json_unpack(value,
                        "{s:{s:s, s:s, s?:o, s:o}}",
                        "new",
                        "name",
                        &name,
                        "type",
                        &type,
                        "error",
                        &error,
                        "external_ids",
                        &external_ids)
            == 0)
            new = TRUE;

        if (old) {
            ovs_interface = g_hash_table_lookup(priv->interfaces, key);
            if (!ovs_interface) {
                _LOGW("Interface '%s' was not seen", key);
            } else if (!new || !nm_streq(ovs_interface->name, name)) {
                old = FALSE;
                _LOGT("removed an '%s' interface: %s%s%s",
                      ovs_interface->type,
                      ovs_interface->name,
                      ovs_interface->connection_uuid ? ", " : "",
                      ovs_interface->connection_uuid ?: "");
                if (nm_streq0(ovs_interface->type, "internal")) {
                    /* Currently, the factory only creates NMDevices for
                     * internal interfaces. Ignore the rest. */
                    g_signal_emit(self,
                                  signals[DEVICE_REMOVED],
                                  0,
                                  ovs_interface->name,
                                  NM_DEVICE_TYPE_OVS_INTERFACE);
                }
            }
            g_hash_table_remove(priv->interfaces, key);
        }

        if (new) {
            ovs_interface  = g_slice_new(OpenvswitchInterface);
            *ovs_interface = (OpenvswitchInterface){
                .name            = g_strdup(name),
                .type            = g_strdup(type),
                .connection_uuid = _connection_uuid_from_external_ids(external_ids),
            };
            g_hash_table_insert(priv->interfaces, g_strdup(key), ovs_interface);
            if (old) {
                _LOGT("changed an '%s' interface: %s%s%s",
                      type,
                      ovs_interface->name,
                      ovs_interface->connection_uuid ? ", " : "",
                      ovs_interface->connection_uuid ?: "");
            } else {
                _LOGT("added an '%s' interface: %s%s%s",
                      ovs_interface->type,
                      ovs_interface->name,
                      ovs_interface->connection_uuid ? ", " : "",
                      ovs_interface->connection_uuid ?: "");
                if (nm_streq0(ovs_interface->type, "internal")) {
                    /* Currently, the factory only creates NMDevices for
                     * internal interfaces. Ignore the rest. */
                    g_signal_emit(self,
                                  signals[DEVICE_ADDED],
                                  0,
                                  ovs_interface->name,
                                  NM_DEVICE_TYPE_OVS_INTERFACE);
                }
            }
            /* The error is a string. No error is indicated by an empty set,
             * because why the fuck not: [ "set": [] ] */
            if (error && json_is_string(error)) {
                g_signal_emit(self,
                              signals[INTERFACE_FAILED],
                              0,
                              ovs_interface->name,
                              ovs_interface->connection_uuid,
                              json_string_value(error));
            }
        }
    }

    /* Ports */
    json_object_foreach (port, key, value) {
        gboolean old = FALSE;
        gboolean new = FALSE;

        if (json_unpack(value, "{s:{}}", "old") == 0)
            old = TRUE;

        if (json_unpack(value,
                        "{s:{s:s, s:o, s:o}}",
                        "new",
                        "name",
                        &name,
                        "external_ids",
                        &external_ids,
                        "interfaces",
                        &items)
            == 0)
            new = TRUE;

        if (old) {
            ovs_port = g_hash_table_lookup(priv->ports, key);
            if (!new || (ovs_port && !nm_streq0(ovs_port->name, name))) {
                old = FALSE;
                _LOGT("removed a port: %s%s%s",
                      ovs_port->name,
                      ovs_port->connection_uuid ? ", " : "",
                      ovs_port->connection_uuid ?: "");
                g_signal_emit(self,
                              signals[DEVICE_REMOVED],
                              0,
                              ovs_port->name,
                              NM_DEVICE_TYPE_OVS_PORT);
            }
            g_hash_table_remove(priv->ports, key);
        }

        if (new) {
            ovs_port  = g_slice_new(OpenvswitchPort);
            *ovs_port = (OpenvswitchPort){
                .name            = g_strdup(name),
                .connection_uuid = _connection_uuid_from_external_ids(external_ids),
                .interfaces      = g_ptr_array_new_with_free_func(g_free),
            };
            _uuids_to_array(ovs_port->interfaces, items);
            g_hash_table_insert(priv->ports, g_strdup(key), ovs_port);
            if (old) {
                _LOGT("changed a port: %s%s%s",
                      ovs_port->name,
                      ovs_port->connection_uuid ? ", " : "",
                      ovs_port->connection_uuid ?: "");
            } else {
                _LOGT("added a port: %s%s%s",
                      ovs_port->name,
                      ovs_port->connection_uuid ? ", " : "",
                      ovs_port->connection_uuid ?: "");
                g_signal_emit(self,
                              signals[DEVICE_ADDED],
                              0,
                              ovs_port->name,
                              NM_DEVICE_TYPE_OVS_PORT);
            }
        }
    }

    /* Bridges */
    json_object_foreach (bridge, key, value) {
        gboolean old = FALSE;
        gboolean new = FALSE;

        if (json_unpack(value, "{s:{}}", "old") == 0)
            old = TRUE;

        if (json_unpack(value,
                        "{s:{s:s, s:o, s:o}}",
                        "new",
                        "name",
                        &name,
                        "external_ids",
                        &external_ids,
                        "ports",
                        &items)
            == 0)
            new = TRUE;

        if (old) {
            ovs_bridge = g_hash_table_lookup(priv->bridges, &key);
            if (!new || (ovs_bridge && !nm_streq0(ovs_bridge->name, name))) {
                old = FALSE;
                _LOGT("removed a bridge: %s%s%s",
                      ovs_bridge->name,
                      ovs_bridge->connection_uuid ? ", " : "",
                      ovs_bridge->connection_uuid ?: "");
                g_signal_emit(self,
                              signals[DEVICE_REMOVED],
                              0,
                              ovs_bridge->name,
                              NM_DEVICE_TYPE_OVS_BRIDGE);
            }
            g_hash_table_remove(priv->bridges, &key);
        }

        if (new) {
            ovs_bridge  = g_slice_new(OpenvswitchBridge);
            *ovs_bridge = (OpenvswitchBridge){
                .bridge_uuid     = g_strdup(key),
                .name            = g_strdup(name),
                .connection_uuid = _connection_uuid_from_external_ids(external_ids),
                .ports           = g_ptr_array_new_with_free_func(g_free),
            };
            _uuids_to_array(ovs_bridge->ports, items);
            g_hash_table_add(priv->bridges, ovs_bridge);
            if (old) {
                _LOGT("changed a bridge: %s%s%s",
                      ovs_bridge->name,
                      ovs_bridge->connection_uuid ? ", " : "",
                      ovs_bridge->connection_uuid ?: "");
            } else {
                _LOGT("added a bridge: %s%s%s",
                      ovs_bridge->name,
                      ovs_bridge->connection_uuid ? ", " : "",
                      ovs_bridge->connection_uuid ?: "");
                g_signal_emit(self,
                              signals[DEVICE_ADDED],
                              0,
                              ovs_bridge->name,
                              NM_DEVICE_TYPE_OVS_BRIDGE);
            }
        }
    }
}

/**
 * ovsdb_got_echo:
 *
 * Only implemented because the specification mandates it. Actual ovsdb hasn't been
 * seen doing this.
 */
static void
ovsdb_got_echo(NMOvsdb *self, json_int_t id, json_t *data)
{
    NMOvsdbPrivate *    priv        = NM_OVSDB_GET_PRIVATE(self);
    nm_auto_decref_json json_t *msg = NULL;
    char *                      reply;
    gboolean                    output_was_empty;

    output_was_empty = priv->output->len == 0;

    msg   = json_pack("{s:I, s:O}", "id", id, "result", data);
    reply = json_dumps(msg, 0);
    g_string_append(priv->output, reply);
    free(reply);

    if (output_was_empty)
        ovsdb_write(self);
}

/**
 * ovsdb_got_msg::
 *
 * Called when a complete JSON object was seen and unmarshalled.
 * Either finishes a method call or processes a method call.
 */
static void
ovsdb_got_msg(NMOvsdb *self, json_t *msg)
{
    NMOvsdbPrivate *priv       = NM_OVSDB_GET_PRIVATE(self);
    json_error_t    json_error = {
        0,
    };
    json_t *            json_id = NULL;
    json_int_t          id      = (json_int_t) -1;
    const char *        method  = NULL;
    json_t *            params  = NULL;
    json_t *            result  = NULL;
    json_t *            error   = NULL;
    OvsdbMethodCall *   call    = NULL;
    OvsdbMethodCallback callback;
    gpointer            user_data;
    gs_free_error GError *local = NULL;

    if (json_unpack_ex(msg,
                       &json_error,
                       0,
                       "{s?:o, s?:s, s?:o, s?:o, s?:o}",
                       "id",
                       &json_id,
                       "method",
                       &method,
                       "params",
                       &params,
                       "result",
                       &result,
                       "error",
                       &error)
        == -1) {
        _LOGW("couldn't grok the message: %s", json_error.text);
        ovsdb_disconnect(self, FALSE, FALSE);
        return;
    }

    if (json_is_number(json_id))
        id = json_integer_value(json_id);

    if (method) {
        /* It's a method call! */
        if (!params) {
            _LOGW("a method call with no params: '%s'", method);
            ovsdb_disconnect(self, FALSE, FALSE);
            return;
        }

        if (nm_streq0(method, "update")) {
            /* This is a update method call. */
            ovsdb_got_update(self, json_array_get(params, 1));
        } else if (nm_streq0(method, "echo")) {
            /* This is an echo request. */
            ovsdb_got_echo(self, id, params);
        } else {
            _LOGW("got an unknown method call: '%s'", method);
        }
        return;
    }

    if (id >= 0) {
        /* This is a response to a method call. */
        if (!priv->calls->len) {
            _LOGE("there are no queued calls expecting response %" G_GUINT64_FORMAT, (guint64) id);
            ovsdb_disconnect(self, FALSE, FALSE);
            return;
        }
        call = &g_array_index(priv->calls, OvsdbMethodCall, 0);
        if (call->call_id != id) {
            _LOGE("expected a response to call %" G_GUINT64_FORMAT ", not %" G_GUINT64_FORMAT,
                  call->call_id,
                  (guint64) id);
            ovsdb_disconnect(self, FALSE, FALSE);
            return;
        }
        /* Cool, we found a corresponding call. Finish it. */

        _LOGT_call("response", call, msg);

        if (!json_is_null(error)) {
            /* The response contains an error. */
            g_set_error(&local,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Error call to OVSDB returned an error: %s",
                        json_string_value(error));
        }

        callback  = call->callback;
        user_data = call->user_data;
        g_array_remove_index(priv->calls, 0);
        callback(self, result, local, user_data);
        priv->num_failures = 0;

        /* Don't progress further commands in case the callback hit an error
         * and disconnected us. */
        if (!priv->conn)
            return;

        /* Now we're free to serialize and send the next command, if any. */
        ovsdb_next_command(self);

        return;
    }

    /* This is a message we are not interested in. */
    _LOGW("got an unknown message, ignoring");
}

/*****************************************************************************/

/* Lower level marshalling and demarshalling of the JSON-RPC traffic on the
 * ovsdb socket. */

static size_t
_json_callback(void *buffer, size_t buflen, void *user_data)
{
    NMOvsdb *       self = NM_OVSDB(user_data);
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);

    if (priv->bufp == priv->input->len) {
        /* No more bytes buffered for decoding. */
        return 0;
    }

    /* Pass one more byte to the JSON decoder. */
    *(char *) buffer = priv->input->str[priv->bufp];
    priv->bufp++;

    return (size_t) 1;
}

/**
 * ovsdb_read_cb:
 *
 * Read out the data available from the ovsdb socket and try to deserialize
 * the JSON. If we see a complete object, pass it upwards to ovsdb_got_msg().
 */
static void
ovsdb_read_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    NMOvsdb *       self   = NM_OVSDB(user_data);
    NMOvsdbPrivate *priv   = NM_OVSDB_GET_PRIVATE(self);
    GInputStream *  stream = G_INPUT_STREAM(source_object);
    GError *        error  = NULL;
    gssize          size;
    json_t *        msg;
    json_error_t    json_error = {
        0,
    };

    size = g_input_stream_read_finish(stream, res, &error);
    if (size == -1) {
        /* ovsdb-server was possibly restarted */
        _LOGW("short read from ovsdb: %s", error->message);
        priv->num_failures++;
        g_clear_error(&error);
        ovsdb_disconnect(self, priv->num_failures <= OVSDB_MAX_FAILURES, FALSE);
        return;
    }

    g_string_append_len(priv->input, priv->buf, size);
    do {
        priv->bufp = 0;
        /* The callback always eats up only up to a single byte. This makes
         * it possible for us to identify complete JSON objects in spite of
         * us not knowing the length in advance. */
        msg = json_load_callback(_json_callback, self, JSON_DISABLE_EOF_CHECK, &json_error);
        if (msg) {
            ovsdb_got_msg(self, msg);
            g_string_erase(priv->input, 0, priv->bufp);
        }
        json_decref(msg);
    } while (msg);

    if (!priv->conn)
        return;

    if (size)
        ovsdb_read(self);
}

static void
ovsdb_read(NMOvsdb *self)
{
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);

    g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(priv->conn)),
                              priv->buf,
                              sizeof(priv->buf),
                              G_PRIORITY_DEFAULT,
                              NULL,
                              ovsdb_read_cb,
                              self);
}

static void
ovsdb_write_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GOutputStream * stream = G_OUTPUT_STREAM(source_object);
    NMOvsdb *       self   = NM_OVSDB(user_data);
    NMOvsdbPrivate *priv   = NM_OVSDB_GET_PRIVATE(self);
    GError *        error  = NULL;
    gssize          size;

    size = g_output_stream_write_finish(stream, res, &error);
    if (size == -1) {
        /* ovsdb-server was possibly restarted */
        _LOGW("short write to ovsdb: %s", error->message);
        priv->num_failures++;
        g_clear_error(&error);
        ovsdb_disconnect(self, priv->num_failures <= OVSDB_MAX_FAILURES, FALSE);
        return;
    }

    if (!priv->conn)
        return;

    g_string_erase(priv->output, 0, size);

    ovsdb_write(self);
}

static void
ovsdb_write(NMOvsdb *self)
{
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);
    GOutputStream * stream;

    if (!priv->output->len)
        return;

    stream = g_io_stream_get_output_stream(G_IO_STREAM(priv->conn));
    if (g_output_stream_has_pending(stream))
        return;

    g_output_stream_write_async(stream,
                                priv->output->str,
                                priv->output->len,
                                G_PRIORITY_DEFAULT,
                                NULL,
                                ovsdb_write_cb,
                                self);
}

/*****************************************************************************/

/* Routines to maintain the ovsdb connection. */

/**
 * ovsdb_disconnect:
 *
 * Clean up the internal state to the point equivalent to before connecting.
 * Apart from clean shutdown this is a good response to unexpected trouble,
 * since the next method call attempt a will trigger reconnect which hopefully
 * puts us back in sync.
 */
static void
ovsdb_disconnect(NMOvsdb *self, gboolean retry, gboolean is_disposing)
{
    NMOvsdbPrivate *    priv = NM_OVSDB_GET_PRIVATE(self);
    OvsdbMethodCall *   call;
    OvsdbMethodCallback callback;
    gpointer            user_data;
    gs_free_error GError *error = NULL;

    nm_assert(!retry || !is_disposing);

    if (!priv->client)
        return;

    _LOGD("disconnecting from ovsdb, retry %d", retry);

    if (retry) {
        if (priv->calls->len != 0)
            g_array_index(priv->calls, OvsdbMethodCall, 0).call_id = CALL_ID_UNSPEC;
    } else {
        nm_utils_error_set_cancelled(&error, is_disposing, "NMOvsdb");

        while (priv->calls->len) {
            call      = &g_array_index(priv->calls, OvsdbMethodCall, priv->calls->len - 1);
            callback  = call->callback;
            user_data = call->user_data;
            g_array_remove_index(priv->calls, priv->calls->len - 1);
            callback(self, NULL, error, user_data);
        }
    }

    priv->bufp = 0;
    g_string_truncate(priv->input, 0);
    g_string_truncate(priv->output, 0);
    g_clear_object(&priv->client);
    g_clear_object(&priv->conn);
    nm_clear_g_free(&priv->db_uuid);
    nm_clear_g_cancellable(&priv->cancellable);

    if (retry)
        ovsdb_try_connect(self);
}

static void
_monitor_bridges_cb(NMOvsdb *self, json_t *result, GError *error, gpointer user_data)
{
    if (error) {
        if (!nm_utils_error_is_cancelled_or_disposing(error)) {
            _LOGI("%s", error->message);
            ovsdb_disconnect(self, FALSE, FALSE);
        }
        return;
    }

    /* Treat the first response the same as the subsequent "update"
     * messages we eventually get. */
    ovsdb_got_update(self, result);
}

static void
_client_connect_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GSocketClient *    client = G_SOCKET_CLIENT(source_object);
    NMOvsdb *          self   = NM_OVSDB(user_data);
    NMOvsdbPrivate *   priv;
    GError *           error = NULL;
    GSocketConnection *conn;

    conn = g_socket_client_connect_finish(client, res, &error);
    if (conn == NULL) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            _LOGI("%s", error->message);

        ovsdb_disconnect(self, FALSE, FALSE);
        g_clear_error(&error);
        return;
    }

    priv       = NM_OVSDB_GET_PRIVATE(self);
    priv->conn = conn;
    g_clear_object(&priv->cancellable);

    ovsdb_read(self);
    ovsdb_next_command(self);
}

/**
 * ovsdb_try_connect:
 *
 * Establish a connection to ovsdb unless it's already established or being
 * established. Queues a monitor command as a very first one so that we're in
 * sync when other commands are issued.
 */
static void
ovsdb_try_connect(NMOvsdb *self)
{
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);
    GSocketAddress *addr;

    if (priv->client)
        return;

    /* TODO: This should probably be made configurable via NetworkManager.conf */
    addr = g_unix_socket_address_new(RUNSTATEDIR "/openvswitch/db.sock");

    priv->client      = g_socket_client_new();
    priv->cancellable = g_cancellable_new();
    g_socket_client_connect_async(priv->client,
                                  G_SOCKET_CONNECTABLE(addr),
                                  priv->cancellable,
                                  _client_connect_cb,
                                  self);
    g_object_unref(addr);

    /* Queue a monitor call before any other command, ensuring that we have an up
     * to date view of existing bridged that we need for add and remove ops. */
    ovsdb_call_method(self,
                      OVSDB_MONITOR,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      0,
                      _monitor_bridges_cb,
                      NULL,
                      TRUE);
}

/*****************************************************************************/

/* Public functions useful for NMDeviceOpenvswitch to maintain the life cycle of
 * their ovsdb entries without having to deal with ovsdb complexities themselves. */

typedef struct {
    NMOvsdbCallback callback;
    gpointer        user_data;
} OvsdbCall;

static void
_transact_cb(NMOvsdb *self, json_t *result, GError *error, gpointer user_data)
{
    OvsdbCall * call = user_data;
    const char *err;
    const char *err_details;
    size_t      index;
    json_t *    value;

    if (error)
        goto out;

    json_array_foreach (result, index, value) {
        if (json_unpack(value, "{s:s, s:s}", "error", &err, "details", &err_details) == 0) {
            g_set_error(&error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Error running the transaction: %s: %s",
                        err,
                        err_details);
            goto out;
        }
    }

out:
    call->callback(error, call->user_data);
    nm_g_slice_free(call);
}

static OvsdbCall *
ovsdb_call_new(NMOvsdbCallback callback, gpointer user_data)
{
    OvsdbCall *call;

    call  = g_slice_new(OvsdbCall);
    *call = (OvsdbCall){
        .callback  = callback,
        .user_data = user_data,
    };
    return call;
}

void
nm_ovsdb_add_interface(NMOvsdb *       self,
                       NMConnection *  bridge,
                       NMConnection *  port,
                       NMConnection *  interface,
                       NMDevice *      bridge_device,
                       NMDevice *      interface_device,
                       NMOvsdbCallback callback,
                       gpointer        user_data)
{
    ovsdb_call_method(self,
                      OVSDB_ADD_INTERFACE,
                      NULL,
                      bridge,
                      port,
                      interface,
                      bridge_device,
                      interface_device,
                      0,
                      _transact_cb,
                      ovsdb_call_new(callback, user_data),
                      FALSE);
}

void
nm_ovsdb_del_interface(NMOvsdb *       self,
                       const char *    ifname,
                       NMOvsdbCallback callback,
                       gpointer        user_data)
{
    ovsdb_call_method(self,
                      OVSDB_DEL_INTERFACE,
                      ifname,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      0,
                      _transact_cb,
                      ovsdb_call_new(callback, user_data),
                      FALSE);
}

void
nm_ovsdb_set_interface_mtu(NMOvsdb *       self,
                           const char *    ifname,
                           guint32         mtu,
                           NMOvsdbCallback callback,
                           gpointer        user_data)
{
    ovsdb_call_method(self,
                      OVSDB_SET_INTERFACE_MTU,
                      ifname,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      mtu,
                      _transact_cb,
                      ovsdb_call_new(callback, user_data),
                      FALSE);
}

/*****************************************************************************/

static void
_clear_call(gpointer data)
{
    OvsdbMethodCall *call = data;

    switch (call->command) {
    case OVSDB_MONITOR:
        break;
    case OVSDB_ADD_INTERFACE:
        g_clear_object(&call->bridge);
        g_clear_object(&call->port);
        g_clear_object(&call->interface);
        g_clear_object(&call->bridge_device);
        g_clear_object(&call->interface_device);
        break;
    case OVSDB_DEL_INTERFACE:
    case OVSDB_SET_INTERFACE_MTU:
        nm_clear_g_free(&call->ifname);
        break;
    }
}

static void
_free_bridge(gpointer data)
{
    OpenvswitchBridge *ovs_bridge = data;

    g_free(ovs_bridge->bridge_uuid);
    g_free(ovs_bridge->name);
    g_free(ovs_bridge->connection_uuid);
    g_ptr_array_free(ovs_bridge->ports, TRUE);
    nm_g_slice_free(ovs_bridge);
}

static void
_free_port(gpointer data)
{
    OpenvswitchPort *ovs_port = data;

    g_free(ovs_port->name);
    g_free(ovs_port->connection_uuid);
    g_ptr_array_free(ovs_port->interfaces, TRUE);
    nm_g_slice_free(ovs_port);
}

static void
_free_interface(gpointer data)
{
    OpenvswitchInterface *ovs_interface = data;

    g_free(ovs_interface->name);
    g_free(ovs_interface->connection_uuid);
    g_free(ovs_interface->type);
    nm_g_slice_free(ovs_interface);
}

static void
nm_ovsdb_init(NMOvsdb *self)
{
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);

    priv->calls = g_array_new(FALSE, TRUE, sizeof(OvsdbMethodCall));
    g_array_set_clear_func(priv->calls, _clear_call);
    priv->input      = g_string_new(NULL);
    priv->output     = g_string_new(NULL);
    priv->bridges    = g_hash_table_new_full(nm_pstr_hash, nm_pstr_equal, _free_bridge, NULL);
    priv->ports      = g_hash_table_new_full(nm_str_hash, g_str_equal, g_free, _free_port);
    priv->interfaces = g_hash_table_new_full(nm_str_hash, g_str_equal, g_free, _free_interface);

    ovsdb_try_connect(self);
}

static void
dispose(GObject *object)
{
    NMOvsdb *       self = NM_OVSDB(object);
    NMOvsdbPrivate *priv = NM_OVSDB_GET_PRIVATE(self);

    ovsdb_disconnect(self, FALSE, TRUE);

    if (priv->input) {
        g_string_free(priv->input, TRUE);
        priv->input = NULL;
    }
    if (priv->output) {
        g_string_free(priv->output, TRUE);
        priv->output = NULL;
    }
    if (priv->calls) {
        g_array_free(priv->calls, TRUE);
        priv->calls = NULL;
    }

    nm_clear_pointer(&priv->bridges, g_hash_table_destroy);
    nm_clear_pointer(&priv->ports, g_hash_table_destroy);
    nm_clear_pointer(&priv->interfaces, g_hash_table_destroy);

    G_OBJECT_CLASS(nm_ovsdb_parent_class)->dispose(object);
}

static void
nm_ovsdb_class_init(NMOvsdbClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = dispose;

    signals[DEVICE_ADDED] = g_signal_new(NM_OVSDB_DEVICE_ADDED,
                                         G_OBJECT_CLASS_TYPE(object_class),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         NULL,
                                         G_TYPE_NONE,
                                         2,
                                         G_TYPE_STRING,
                                         G_TYPE_UINT);

    signals[DEVICE_REMOVED] = g_signal_new(NM_OVSDB_DEVICE_REMOVED,
                                           G_OBJECT_CLASS_TYPE(object_class),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL,
                                           NULL,
                                           NULL,
                                           G_TYPE_NONE,
                                           2,
                                           G_TYPE_STRING,
                                           G_TYPE_UINT);

    signals[INTERFACE_FAILED] = g_signal_new(NM_OVSDB_INTERFACE_FAILED,
                                             G_OBJECT_CLASS_TYPE(object_class),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             NULL,
                                             G_TYPE_NONE,
                                             3,
                                             G_TYPE_STRING,
                                             G_TYPE_STRING,
                                             G_TYPE_STRING);
}
