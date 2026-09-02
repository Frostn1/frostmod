"""What each offset in offsets.h *is*, expressed as something a new build still has.

Order matters only through `deps`; the resolver topologically sorts. Every entry
carries the offsets.h symbol it feeds so the report reads as a diff of that header.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from rules import (
    AfterString, Aob, Array, ArrayOf, BusPointer, CallerOwning, ClusterBase, Const,
    CountOf, FunctionWithGlobals, GlobalNearString, GlobalsOfExcept, ImportUser,
    MostRefsAmong, Rel, Rule, SharedGlobals, SiteInFunction, SiteOfStringRef,
    StrideOf, Strings,
)


@dataclass
class Anchor:
    key: str                 # offsets.h symbol
    rule: Rule
    what: str                # one line, for the report
    group: str = "misc"
    consumer: str = "frostmod"   # frostmod | mxb-app
    kind: str = "rva"            # rva | value


# The scanner prologue and the bike-apply prologue already lived in offsets.h as AOBs;
# they stay the fallback for the two functions with no strings of their own.
SIG_SCAN_FOLDER = "40 53 56 57 41 54 41 55 41 56 48 81 EC F8 07 00 00 48 8B 05"
SIG_BIKE_APPLY = "40 53 56 57 41 57 B8 C8 20 00 00 E8 ? ? ? ? 48 2B E0 48 8B 05"

MXB: list[Anchor] = [
    # ---- content loading ----------------------------------------------------
    Anchor("RVA_CONTENT_INIT",
           Strings("unnamedProfile", "texture_quality", "lastprofile"),
           "boot content-load + app-init (the reload target)", "content"),
    Anchor("RVA_SCAN_FOLDER", Aob(SIG_SCAN_FOLDER),
           "generic VFS directory walker", "content"),
    # The scanner touches exactly three registry globals and nothing else; `expect`
    # makes a build that adds a fourth fail loudly instead of silently shifting.
    Anchor("RVA_REG_COUNT", GlobalsOfExcept("RVA_SCAN_FOLDER", 0, expect=3),
           "content registry entry count", "content"),
    Anchor("RVA_REG_BASE", GlobalsOfExcept("RVA_SCAN_FOLDER", 1, expect=3),
           "content registry entry array", "content"),

    # ---- track list ---------------------------------------------------------
    Anchor("RVA_TRACK_LIST", Array(r"%stracks\%s%s\%s.amb"),
           "qword: pointer to the track array", "tracks"),
    Anchor("TRACK_STRIDE", StrideOf("RVA_TRACK_LIST"),
           "bytes per track entry", "tracks", kind="value"),
    Anchor("RVA_TRACK_COUNT", CountOf("RVA_TRACK_LIST"),
           "int32 track count", "tracks"),
    Anchor("RVA_TRK_DATA_LOADER", MostRefsAmong(r"%stracks\%s%s\%s.amb", "RVA_TRACK_LIST"),
           "track data loader (ecx = index)", "tracks"),
    Anchor("RVA_TRK_LOAD_ENTER",
           CallerOwning("RVA_TRK_DATA_LOADER", refs=("RVA_TRACK_LIST",),
                        without=("testing",)),
           "load track + enter session", "tracks"),

    # ---- bike list ----------------------------------------------------------
    Anchor("RVA_BIKE_LIST", Array(r"%sbikes\%s\%s.cfg"),
           "qword: pointer to the bike array", "bikes"),
    Anchor("BIKE_STRIDE", StrideOf("RVA_BIKE_LIST"),
           "bytes per bike entry", "bikes", kind="value"),
    Anchor("RVA_BIKE_COUNT", CountOf("RVA_BIKE_LIST"),
           "int32 bike count", "bikes"),
    Anchor("RVA_BIKE_APPLY", Aob(SIG_BIKE_APPLY),
           "bike/session apply loader", "bikes"),

    # ---- networking ---------------------------------------------------------
    Anchor("RVA_NET_RECVFROM_W", ImportUser("recvfrom"),
           "recvfrom wrapper", "net"),
    Anchor("RVA_NET_SENDTO_W", ImportUser("sendto"),
           "sendto wrapper", "net"),
    Anchor("RVA_NET_RESOLVE_HOST", ImportUser("getaddrinfo", without_strings=("127.0.0.1",)),
           "host:port -> getaddrinfo", "net"),

    # ---- master server / server list ---------------------------------------
    # The server-side handler owns the same four opcodes plus the ones only a master
    # speaks, so excluding GETLIST is what separates the client half from it.
    Anchor("RVA_MP_MSG_HANDLER",
           Strings("HOSTED", "ENDPOINT", "PING", "ECHO", without=("GETLIST", "LOGIN")),
           "client opcode handler; HOSTED = server-list reply", "master"),
    Anchor("RVA_MP_REFRESH_DRV", Strings("GETLIST2", "LIST2", "KEEPUSER"),
           "sends GETLIST to the masters on a refresh timer", "master"),
    # The reply buffer is written from exactly one place, which is what tells it apart
    # from the parser scratch that sits beside it.
    Anchor("RVA_MP_LIST_BLOB", GlobalNearString("RVA_MP_MSG_HANDLER", "HOSTED", max_fanout=1),
           "HOSTED payload text buffer", "master"),
    Anchor("RVA_MP_ENDPOINT_CNT",
           SharedGlobals("RVA_MP_MSG_HANDLER", "RVA_MP_REFRESH_DRV", 0, expect=2),
           "master endpoint count", "master"),
    Anchor("RVA_MP_ENDPOINT_BASE",
           SharedGlobals("RVA_MP_MSG_HANDLER", "RVA_MP_REFRESH_DRV", 1, expect=2),
           "master endpoint array (stride 0x28, cap 10)", "master"),
    Anchor("RVA_MP_ENDPOINT_ADD",
           FunctionWithGlobals("RVA_MP_ENDPOINT_CNT", "RVA_MP_ENDPOINT_BASE"),
           "append a MASTER endpoint", "master"),

    # ---- server browser screens --------------------------------------------
    Anchor("RVA_SB_LAN_CMD", Strings("ID_FILTEREMTPY", "ID_REFRESHLIST"),
           "LAN browser: clear + build + populate", "browser"),
    Anchor("RVA_SB_WORLD_CMD", Strings("ID_QUALIFYPRACTICE", "ID_SPECTATE", "ID_JOIN"),
           "world browser: string dispatch", "browser"),
    Anchor("RVA_SB_REFRESHLIST", SiteOfStringRef("RVA_SB_LAN_CMD", "ID_REFRESHLIST", delta=-7),
           "ID_REFRESHLIST branch (LAN)", "browser"),
    Anchor("RVA_SB_POPULATE_LOOP", SiteInFunction("RVA_SB_LAN_CMD", "44 38 25 ? ? ? ? 74 ? 49 63"),
           "per-server populate loop top (hook site)", "browser"),
    Anchor("SB_STATE_BASE", ClusterBase("RVA_SB_LAN_CMD", gap=0x80),
           "base of the browser's state block", "browser", kind="value"),
    Anchor("RVA_SB_CONNECTED_FLAG", Rel("SB_STATE_BASE", 0x08),
           "master/list ready (0/1)", "browser"),
    Anchor("RVA_SB_FILTER_FLAGS", Rel("SB_STATE_BASE", 0x2C),
           "bit0 hide-empty, bit1 hide-full", "browser"),
    Anchor("RVA_SB_DISPLAY_COUNT", Rel("SB_STATE_BASE", 0x30),
           "filtered/displayed row count", "browser"),
    Anchor("RVA_SB_INFO_INDEX", Rel("SB_STATE_BASE", 0x3C),
           "SERVERINFO index (-1)", "browser"),
    Anchor("RVA_SB_RAW_COUNT", Rel("SB_STATE_BASE", 0x40),
           "raw server count (loop bound)", "browser"),
    Anchor("RVA_SB_NAME_FILTER", Rel("SB_STATE_BASE", 0x48),
           "uppercased name-filter string", "browser"),
    Anchor("RVA_SB_SELECTED_INDEX", Rel("SB_STATE_BASE", 0xB0),
           "selected row", "browser"),
    # The rows the browser paints are owned by the master-server module, not by the
    # screen, so the array is anchored off the refresh driver.
    Anchor("RVA_SB_ENTRIES", ArrayOf("RVA_MP_REFRESH_DRV"),
           "server-list entry array", "browser"),
    Anchor("SBE_STRIDE", StrideOf("RVA_SB_ENTRIES"),
           "bytes per server-list entry", "browser", kind="value"),

    # ---- engine command bus -------------------------------------------------
    Anchor("RVA_CMD_BUS_PTR", BusPointer(),
           "engine command-bus function pointer", "bus"),

    # ---- reload plumbing ----------------------------------------------------
    Anchor("RVA_RELOAD_STR", AfterString("mods"),
           "empty string handed to the DIR-shaped loaders for the game dir", "content"),
    Anchor("RVA_RELOAD_MODS", GlobalNearString("RVA_CONTENT_INIT", "mods", max_fanout=60),
           "the mods path buffer the DIR-shaped loaders are handed", "content"),

    # ---- what MXB App itself reaches into ----------------------------------
    Anchor("APP_LOADER_OFFSET", Strings(r"%sprofiles\%s\profile.ini.bak"),
           "customization loader MXB App starts a thread on",
           "mxb-app", consumer="mxb-app"),
    Anchor("APP_PROFILE_SCREEN", Strings("ID_GUIDCOPY", "ID_CURPROFILE"),
           "profile screen (anchors the GUID buffer)", "mxb-app", consumer="mxb-app"),
    Anchor("APP_GUID_OFFSET",
           GlobalNearString("APP_PROFILE_SCREEN", "ID_GUIDCOPY", max_fanout=20, window=0x200),
           "local player GUID buffer", "mxb-app", consumer="mxb-app"),
]

# Struct field offsets and protocol constants: meaning, not addresses. Nothing in the
# image says "+0x4C0 is the cfg name", so these are carried from the baseline and
# always reported as carried — never as re-derived.
# The reader helpers sit in one contiguous module the message handler calls into, so
# the report points at that handler as the place to start looking.
_NEAR = {"RVA_MP_READ_INIT": "RVA_MP_MSG_HANDLER", "RVA_MP_READ_U32": "RVA_MP_MSG_HANDLER",
         "RVA_MP_READ_STR": "RVA_MP_MSG_HANDLER"}

MXB_CARRIED: list[Anchor] = [
    Anchor(k, Const(why, _NEAR.get(k)), what, group,
           kind="value" if k.startswith(("TRK_", "BIKE_", "SBE_", "CMD_", "SESSION", "OFF_", "MXB_")) else "rva")
    for k, why, what, group in [
        ("TRK_FOLDER", "struct field", "track entry: folder/id", "tracks"),
        ("TRK_NAME", "struct field", "track entry: display name", "tracks"),
        ("TRK_SHORT", "struct field", "track entry: short name", "tracks"),
        ("TRK_IMAGE", "struct field", "track entry: preview image", "tracks"),
        ("TRK_RESOLVER_NAME", "struct field", "track entry: resolver name", "tracks"),
        ("BIKE_FOLDER", "struct field", "bike entry: folder/id", "bikes"),
        ("BIKE_CFGNAME", "struct field", "bike entry: cfg name", "bikes"),
        ("SBE_NAME", "struct field", "server entry: name", "browser"),
        ("SBE_MAXPLAYERS", "struct field", "server entry: max players", "browser"),
        ("CMD_JOIN", "protocol id", "bus cmd that initiates the connect", "bus"),
        ("CMD_SESSION_START", "protocol id", "bus cmd handed the settings block", "bus"),
        ("CMD_CFG_READ_INT", "protocol id", "bus cmd: read an int out of the ini", "bus"),
        ("SESSION_CFG_SIZE", "struct size", "session settings block size", "bus"),
        ("OFF_OVERJUMP_DISABLED", "struct field", "session settings: overjump disabled", "bus"),
        ("MXB_DEFAULT_PORT", "protocol constant", "default server port", "net"),
        ("RVA_MP_READ_INIT", "leaf helper: no strings, imports or globals to anchor on",
         "reader: init", "master"),
        ("RVA_MP_READ_U32", "leaf helper: no strings, imports or globals to anchor on",
         "reader: read_u32", "master"),
        ("RVA_MP_READ_STR", "leaf helper: no strings, imports or globals to anchor on",
         "reader: read_str", "master"),
    ]
]

# GP Bikes is the same engine with the same literals, so it uses the same anchor set —
# only the baseline differs. Confirmed: the rules find its RVA_CONTENT_INIT (0xfb650)
# and RVA_SCAN_FOLDER (0x18f150) unaided, which are the values offsets.h reached by hand.
TITLES = {"mxb": MXB + MXB_CARRIED, "gpb": MXB + MXB_CARRIED}
