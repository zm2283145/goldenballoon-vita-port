#!/usr/bin/env python3
"""Pin the browser Online Room key sets to the Worker's match wire schema.

`dist/web/online/*.js` (the browser room client) and `services/party/src/match`
(the Worker's wire schema) are edited independently and have drifted apart
three times, most recently in the 1.6.0 window:

  * `/api/ops/health` grew the `partyNativeCreate` and `turnMint` reservation
    counters while an exact-equality consumer still pinned the old bucket set;
  * the lobby wire moved to `schemaVersion` 2 with six session-configuration
    fields, and the browser validator still pins 1 and refuses the six;
  * create/join responses grew a top-level `iceServers`, which the browser's
    exact-key wire check refuses outright.

The last two are the same open gap, recorded as ruling R36: the Online Room
surface ships dark, and teaching the client the v2 wire is landing-reviewed
custody work reserved for the owner. This gate does not close that gap. It
extracts both vocabularies from the sources themselves and compares them member
by member, with the R36 members named in the allowlists below. An allowlist
entry that no longer matches a real member on the side it claims is a failure,
so the gap cannot silently change size in either direction.

Source-only: no ROM, no build, no network, no Node, no service.
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "services/party/src"
CLIENT = ROOT / "dist/web/online"
CAPACITY_GATE = ROOT / "tests/check_party_capacity.py"

SOURCE_FILES = {
    "protocol": SERVICE / "match/protocol.ts",
    "match_room": SERVICE / "match/match-room.ts",
    "signaling": SERVICE / "match/signaling.ts",
    "worker": SERVICE / "worker.ts",
    "budget": SERVICE / "party-budget.ts",
    "live_state": CLIENT / "online-room-live-state.js",
    "room": CLIENT / "online-room.js",
    "signal_client": CLIENT / "match-signal-client.js",
    "capacity_gate": CAPACITY_GATE,
}

# --- R36 allowlist -----------------------------------------------------------
# Wire members the shipped browser client deliberately does not speak. Each
# entry must still name a real member of the side it claims; the comparison
# below reports an entry that has gone stale exactly as loudly as new drift.

# R36 CLOSED 2026-09-08. The browser client speaks the v2 wire: it accepts
# schemaVersion 2, the delivered iceServers envelope member, and the six
# session-configuration lobby fields. The allowlists stay here, empty, so a
# regression reports as new drift against an empty allowance instead of quietly
# reintroducing a gap that once had a ruling attached to it.
R36_LOBBY_SCHEMA_ONLY = frozenset()
R36_ENVELOPE_SCHEMA_ONLY = frozenset()

# Command types the schema defines that no dist/web/online path issues.
COMMANDS_NOT_SENT_BY_BROWSER = {
    "join": "MatchRoom builds the join command itself inside /join; the "
            "browser posts the invite, never the command",
    "disconnect": "native transport lifecycle; the browser leaves instead",
    "reconnect": "native transport lifecycle; the browser reopens its state "
                 "socket without a command",
    "begin_loading": "race lifecycle; the browser room stops at Ready",
    "ack_loaded": "race lifecycle; the browser room stops at Ready",
    "begin_race": "race lifecycle; the browser room stops at Ready",
    "cancel_loading": "race lifecycle; the browser room stops at Ready",
    "publish_results": "race lifecycle; the browser room stops at Ready",
    "rematch": "race lifecycle; the browser room stops at Ready",
    "transfer_leader": "no browser control surface offers leader transfer",
    "set_mode": "R36 v2 session configuration, unknown to the browser client",
    "set_config_track": "R36 v2 session configuration, unknown to the browser "
                        "client",
    "set_cup": "R36 v2 session configuration, unknown to the browser client",
}

# Socket close codes the browser treats as ordinary transport loss and retries.
# Only the 4000 class is a terminal verdict (terminalLiveCloseCode).
NON_TERMINAL_CLOSE_CODES = frozenset({1011, 4001, 4003, 4008, 4009})

# Recovery codes online-room.js branches on that no service token produces:
# the client's own default when an error carries no typed code.
CLIENT_ONLY_RECOVERY_CODES = frozenset({"service_unavailable"})


class Drift(AssertionError):
    """A parity failure, or a source this gate can no longer read."""


# --- source scanning ---------------------------------------------------------

def scrub(source: str) -> str:
    """Blank out JS/TS comments, keeping every byte offset and line break.

    Comment prose carries apostrophes and braces that would otherwise unbalance
    the bracket scans below, so comments are erased before any bracket scan and
    the scans see only code and string literals.
    """
    out = list(source)
    quote = ""
    index = 0
    while index < len(source):
        char = source[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in "\"'`":
            quote = char
        elif source.startswith("//", index):
            while index < len(source) and source[index] != "\n":
                out[index] = " "
                index += 1
            continue
        elif source.startswith("/*", index):
            end = source.find("*/", index)
            if end < 0:
                raise Drift(f"unterminated block comment at offset {index}")
            for position in range(index, end + 2):
                if out[position] != "\n":
                    out[position] = " "
            index = end + 2
            continue
        index += 1
    return "".join(out)


def flatten(source: str) -> str:
    """Collapse whitespace so multi-line declarations match as one string."""
    return re.sub(r"\s+", " ", source)


def balanced(source: str, start: int) -> str:
    """The body between the bracket at `start` and its partner.

    Quoted spans are skipped whole, so a template literal's `${...}` and a URL
    inside a string cannot unbalance the scan.
    """
    opener = source[start]
    closer = {"{": "}", "[": "]", "(": ")"}[opener]
    depth = 0
    quote = ""
    index = start
    while index < len(source):
        char = source[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in "\"'`":
            quote = char
        elif char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
        index += 1
    raise Drift(f"unterminated {opener!r} at offset {start}")


def split_items(body: str) -> list[str]:
    """Comma-separated entries of an object/array body, at its own depth."""
    items: list[str] = []
    depth = 0
    quote = ""
    start = 0
    index = 0
    while index < len(body):
        char = body[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in "\"'`":
            quote = char
        elif char in "{[(":
            depth += 1
        elif char in "}])":
            depth -= 1
        elif char == "," and depth == 0:
            items.append(body[start:index])
            start = index + 1
        index += 1
    items.append(body[start:])
    return [item.strip() for item in items if item.strip()]


def object_keys(body: str) -> set[str]:
    """Property names an object-literal body declares, ignoring spreads."""
    keys: set[str] = set()
    for item in split_items(body):
        if item.startswith("..."):
            continue
        named = re.match(r"([A-Za-z_]\w*)\s*:", item)
        shorthand = re.fullmatch(r"[A-Za-z_]\w*", item)
        if named:
            keys.add(named.group(1))
        elif shorthand:
            keys.add(item)
        else:
            raise Drift(f"unreadable object-literal entry: {item[:60]!r}")
    return keys


def string_literals(text: str) -> list[str]:
    return re.findall(r'"([^"\\]*)"', text)


def literal_after(source: str, anchor: str, bracket: str, label: str) -> str:
    """The bracketed body that follows the first `anchor` in `source`."""
    position = source.find(anchor)
    if position < 0:
        raise Drift(f"{label}: anchor {anchor!r} is gone")
    start = source.find(bracket, position)
    if start < 0:
        raise Drift(f"{label}: no {bracket!r} after {anchor!r}")
    return balanced(source, start)


def named_array(source: str, name: str, label: str) -> list[str]:
    """A `const NAME = [...]` / `Object.freeze([...])` string array."""
    match = re.search(
        rf"\b{name}\s*=\s*(?:Object\.freeze\(|new Set<[^>]*>\()?\s*\[", source)
    if match is None:
        raise Drift(f"{label}: array {name} is gone")
    values = string_literals(balanced(source, match.end() - 1))
    if not values:
        raise Drift(f"{label}: array {name} yielded no members")
    return values


def ts_interface_fields(source: str, name: str) -> list[str]:
    body = literal_after(source,
                         f"export interface {name} {{", "{", f"interface {name}")
    fields = [match.group(1) for line in body.splitlines()
              if not line.strip().startswith("//")
              and (match := re.match(r"\s+([A-Za-z_]\w*)\??\s*:", line))]
    if not fields:
        raise Drift(f"interface {name} yielded no fields")
    return fields


def ts_union_members(source: str, declaration: str, label: str) -> list[str]:
    position = source.find(declaration)
    if position < 0:
        raise Drift(f"{label}: declaration {declaration!r} is gone")
    end = source.find(";", position)
    members = string_literals(source[position:end])
    if not members:
        raise Drift(f"{label}: union {declaration!r} yielded no members")
    return members


def function_body(source: str, signature: str, label: str) -> str:
    return literal_after(source, signature, "{", label)


# --- schema vocabulary -------------------------------------------------------

def destructured_names(source: str, subject: str, label: str) -> set[str]:
    """Fields a `const {a: _a, ...rest} = subject` projection removes."""
    match = re.search(r"const \{([^}]*)\.\.\.\w+\} = " + re.escape(subject),
                      flatten(source))
    if match is None:
        raise Drift(f"{label}: no rest-destructure of {subject}")
    names = re.findall(r"([A-Za-z_]\w*)\s*:", match.group(1))
    if not names:
        raise Drift(f"{label}: {subject} destructure removes nothing")
    return set(names)


def schema_vocabulary(sources: dict[str, str]) -> dict[str, object]:
    protocol = sources["protocol"]
    match_room = sources["match_room"]
    public_body = literal_after(
        function_body(match_room, "function publicRoom(", "publicRoom"),
        "return {", "{", "publicRoom return")
    hidden_lobby = destructured_names(match_room, "record.lobby", "publicRoom")
    hidden_member = destructured_names(match_room, "member;", "publicRoom")
    vocabulary: dict[str, object] = {
        "state": object_keys(public_body),
        "lobby": set(ts_interface_fields(protocol, "MatchLobbyV1")) - hidden_lobby,
        "member": set(ts_interface_fields(protocol, "MatchMember")) - hidden_member,
        "seat": set(ts_interface_fields(protocol, "MatchSeat")),
        "control": set(ts_interface_fields(protocol, "MatchStep")),
        "compatibility": set(ts_interface_fields(protocol,
                                                 "MatchCompatibilityV1")),
        "command_request": set(ts_interface_fields(protocol,
                                                   "MatchCommandRequestV1")),
        "phases": set(ts_union_members(protocol, "export type MatchPhase =",
                                       "MatchPhase")),
        "errors": set(ts_union_members(protocol, "export type MatchError =",
                                       "MatchError")),
        "commands": set(named_array(protocol, "MATCH_COMMAND_TYPES", "protocol")),
    }
    stored = literal_after(protocol, "export interface StoredMatchRoomV1 {",
                           "{", "StoredMatchRoomV1")
    version = re.search(r"schemaVersion:\s*(\d+)", stored)
    if version is None:
        raise Drift("StoredMatchRoomV1 no longer pins a schemaVersion")
    vocabulary["schema_version"] = int(version.group(1))
    closed = re.search(r"closedReason:\s*([^;]+);", stored)
    if closed is None:
        raise Drift("StoredMatchRoomV1 no longer declares closedReason")
    vocabulary["closed_reasons"] = set(string_literals(closed.group(1)))
    if not vocabulary["closed_reasons"]:
        raise Drift("StoredMatchRoomV1 closedReason yielded no tokens")
    if vocabulary["commands"] != set(ts_union_members(
            protocol, "export type MatchCommandType =", "MatchCommandType")):
        raise Drift("MATCH_COMMAND_TYPES and MatchCommandType disagree")
    return vocabulary


def schema_envelopes(sources: dict[str, str],
                     state_keys: set[str]) -> list[tuple[str, set[str]]]:
    """Every top-level match-state body the browser can receive."""
    envelopes: list[tuple[str, set[str]]] = []
    for label, source, anchor in (
            ("worker match response", sources["worker"], "json({...state,"),
            ("room-object state delivery", sources["match_room"],
             "{...publicRoom(record),")):
        found = 0
        cursor = 0
        while (position := source.find(anchor, cursor)) >= 0:
            start = source.find("{", position)
            envelopes.append((f"{label} at offset {position}",
                              state_keys | object_keys(balanced(source, start))))
            found += 1
            cursor = position + len(anchor)
        if not found:
            raise Drift(f"{label}: no {anchor!r} site remains")
    plain = (sources["match_room"].count("json(publicRoom(record))") +
             sources["match_room"].count("JSON.stringify(publicRoom(record))"))
    if not plain:
        raise Drift("room object no longer delivers a bare publicRoom body")
    envelopes.append((f"bare publicRoom body ({plain} sites)", set(state_keys)))
    return envelopes


def schema_close_reasons(source: str) -> set[tuple[int, str]]:
    pairs = {(int(code), reason) for code, reason in re.findall(
        r"close(?:Socket|EndpointSockets)\([^;]*?(\d{4}), \"(\w+)\"\)",
        flatten(source))}
    if not pairs:
        raise Drift("match-room.ts emits no readable socket close reasons")
    return pairs


def schema_error_tokens(sources: dict[str, str], errors: set[str],
                        close_reasons: set[str]) -> set[str]:
    tokens = set(errors) | set(close_reasons)
    for name in ("worker", "match_room"):
        found = re.findall(r"\{error: \"(\w+)\"", sources[name])
        if not found:
            raise Drift(f"{name}.ts emits no readable typed error bodies")
        tokens.update(found)
    return tokens


def schema_signal_messages(source: str) -> dict[str, set[str]]:
    """Per-type extra keys `parseMatchSignalMessage` accepts from a client."""
    flat = flatten(source)
    messages: dict[str, set[str]] = {}
    for match in re.finditer(
            r"((?:item\.type === \"\w+\"(?: \|\| )?)+)\)? && exactKeys\(item, "
            r"(\[[^\]]*\])", flat):
        extras = set(string_literals(match.group(2)))
        for name in re.findall(r"\"(\w+)\"", match.group(1)):
            messages[name] = extras
    if not messages:
        raise Drift("signaling.ts yielded no client message shapes")
    return messages


def schema_signal_service_messages(source: str) -> dict[str, set[str]]:
    """Per-type key sets the room object pushes down a signaling socket."""
    messages: dict[str, set[str]] = {}
    cursor = 0
    anchor = "JSON.stringify({protocolVersion: 1, type: \""
    while (position := source.find(anchor, cursor)) >= 0:
        start = source.find("{", position)
        keys = object_keys(balanced(source, start))
        name = re.search(r"type: \"(\w+)\"", source[position:position + 200])
        if name is None:
            raise Drift(f"unnamed signaling push at offset {position}")
        messages[name.group(1)] = keys
        cursor = position + len(anchor)
    if not messages:
        raise Drift("match-room.ts pushes no readable signaling messages")
    return messages


def schema_health(sources: dict[str, str]) -> tuple[set[str], set[str]]:
    budget = sources["budget"]
    operations = set(named_array(budget, "BUDGET_OPERATIONS", "party-budget"))
    stored = re.search(r"\[\.\.\.BUDGET_OPERATIONS, ([^\]]*)\]", budget)
    if stored is None:
        raise Drift("party-budget.ts no longer widens the reservation buckets")
    buckets = operations | set(string_literals(stored.group(1)))
    handler = budget.find('url.pathname === "/health"')
    if handler < 0:
        raise Drift("party-budget.ts no longer serves /health")
    envelope = object_keys(literal_after(budget[handler:], "return json(", "{",
                                         "/health body"))
    if not envelope:
        raise Drift("/health body yielded no keys")
    # The ops route wraps the budget object's body before it reaches an
    # operator, so the served shape is the object's keys plus the Worker's.
    envelope |= object_keys(literal_after(sources["worker"],
                                          "json({...snapshot,", "{",
                                          "ops health wrapper"))
    return buckets, envelope


# --- client vocabulary -------------------------------------------------------

def client_vocabulary(sources: dict[str, str]) -> dict[str, object]:
    live = sources["live_state"]
    room = sources["room"]
    # "wire_lobby" is what the service may send; "lobby" stays the canonical
    # shape frozenState() projects. "ice" is the delivered iceServers envelope
    # member. Both arrived with the v2 wire.
    names = {"state": "PUBLIC_STATE_KEYS", "identity": "IDENTITY_KEYS",
             "invite": "INVITE_KEYS", "ice": "ICE_KEYS",
             "wire_lobby": "WIRE_LOBBY_KEYS", "lobby": "LOBBY_KEYS",
             "member": "MEMBER_KEYS", "seat": "SEAT_KEYS",
             "control": "CONTROL_KEYS", "compatibility": "COMPATIBILITY_KEYS"}
    vocabulary: dict[str, object] = {
        role: set(named_array(live, name, "live-state"))
        for role, name in names.items()
    }
    phases = re.search(r"LOBBY_PHASE = Object\.freeze\(\{", live)
    if phases is None:
        raise Drift("live-state.js no longer declares LOBBY_PHASE")
    vocabulary["phases"] = object_keys(balanced(live, phases.end() - 1))
    # The pin moved from a literal comparison to a named constant when the
    # client learned the v2 wire. Accept either spelling: this gate compares
    # the VALUE against the schema, and failing because a constant was given a
    # name would be measuring code shape rather than compatibility.
    version = re.search(r"MATCH_STATE_SCHEMA_VERSION\s*=\s*(\d+)", live)
    if version is None:
        version = re.search(r"value\.schemaVersion !== (\d+)", live)
    if version is None:
        raise Drift("live-state.js no longer pins a schemaVersion")
    vocabulary["schema_version"] = int(version.group(1))
    closed = re.search(r"!\[([^\]]*)\]\.includes\(value\.closedReason\)", live)
    if closed is None:
        raise Drift("live-state.js no longer bounds closedReason")
    vocabulary["closed_reasons"] = set(string_literals(closed.group(1)))
    vocabulary["command_request"] = object_keys(literal_after(
        function_body(room, "async function sendLiveCommand(", "sendLiveCommand"),
        "const body =", "{", "command request body"))
    vocabulary["commands"] = client_command_types(room)
    vocabulary["command_result_reads"] = set(re.findall(
        r"result[?]?\.(\w+)",
        function_body(room, "async function sendLiveCommand(", "sendLiveCommand")))
    vocabulary["recovery_codes"] = set(re.findall(
        r"code === \"(\w+)\"",
        function_body(room, "function failureSlug(", "failureSlug")))
    if not vocabulary["recovery_codes"]:
        raise Drift("failureSlug branches on no typed recovery code")
    vocabulary["terminal_closes"] = client_terminal_closes(room)
    return vocabulary


def client_command_types(source: str) -> set[str]:
    """Command types reaching sendLiveCommand(), literal or via a local."""
    types: set[str] = set()
    calls = 0
    for match in re.finditer(r"sendLiveCommand\(", source):
        if "function " in source[max(0, match.start() - 32):match.start()]:
            continue  # the declaration itself, not a call site
        argument = split_items(balanced(source, match.end() - 1))
        if not argument:
            raise Drift("sendLiveCommand() called with no command type")
        first = argument[0]
        calls += 1
        if '"' in first:
            types.update(string_literals(first))
            continue
        if not re.fullmatch(r"[A-Za-z_]\w*", first):
            raise Drift(f"unreadable sendLiveCommand argument: {first[:60]!r}")
        bindings = re.findall(rf"const {first} = ([^;]*);", source[:match.start()])
        if not bindings:
            raise Drift(f"sendLiveCommand({first}) has no readable binding")
        resolved = string_literals(bindings[-1])
        if not resolved:
            raise Drift(f"binding for sendLiveCommand({first}) names no type")
        types.update(resolved)
    if calls < 2:
        raise Drift("online-room.js issues no readable match commands")
    return types


def client_terminal_closes(source: str) -> set[tuple[int, str]]:
    body = function_body(source, "function terminalLiveCloseCode(",
                         "terminalLiveCloseCode")
    code = re.search(r"code !== (\d+)", body)
    if code is None:
        raise Drift("terminalLiveCloseCode no longer gates on a close code")
    reasons = re.findall(r"reason === \"(\w+)\"", body)
    if not reasons:
        raise Drift("terminalLiveCloseCode names no terminal close reason")
    return {(int(code.group(1)), reason) for reason in reasons}


def client_accepted_envelopes(sources: dict[str, str],
                              client: dict[str, object]) -> list[set[str]]:
    """The exact top-level key combinations validWireKeys() admits."""
    body = function_body(sources["live_state"], "function validWireKeys(",
                         "validWireKeys")
    combinations: list[set[str]] = []
    for arguments in re.findall(r"exactKeys\(value, ([^)]*)\)", body):
        keys: set[str] = set()
        for name in re.findall(r"([A-Z_]+_KEYS)", arguments):
            role = {"PUBLIC_STATE_KEYS": "state", "IDENTITY_KEYS": "identity",
                    "INVITE_KEYS": "invite", "ICE_KEYS": "ice"}.get(name)
            if role is None:
                raise Drift(f"validWireKeys admits unknown key set {name}")
            keys |= client[role]
        if not keys:
            raise Drift("validWireKeys admits an empty key combination")
        combinations.append(keys)
    if len(combinations) < 2:
        raise Drift("validWireKeys no longer enumerates wire combinations")
    return combinations


def client_signal_messages(source: str) -> tuple[dict[str, set[str]], set[str]]:
    """Per-type extra keys the client sends, and its outgoing envelope keys."""
    body = function_body(source, "function clientMessage(", "clientMessage")
    flat = flatten(body)
    messages: dict[str, set[str]] = {}
    for match in re.finditer(
            r"((?:value\.type === \"\w+\"(?: \|\| )?)+)\)? && exact\(value, "
            r"\[\.\.\.common,([^\]]*)\]\)", flat):
        extras = set(string_literals(match.group(2)))
        for name in re.findall(r"\"(\w+)\"", match.group(1)):
            messages[name] = extras
    if not messages:
        raise Drift("match-signal-client.js sends no readable message shapes")
    envelope = object_keys(literal_after(flat, "return extra ?", "{",
                                         "outgoing signal envelope"))
    return messages, envelope


def client_signal_service_messages(source: str) -> dict[str, set[str]]:
    """Per-type key sets parseServerMessage() accepts, service-originated."""
    flat = flatten(function_body(source, "function parseServerMessage(",
                                 "parseServerMessage"))
    messages: dict[str, set[str]] = {}
    for match in re.finditer(
            r"value\.type === \"(\w+)\" && (?:generation === 0 && )?"
            r"exact\(value, (\[\"[^\]]*\])\)", flat):
        messages[match.group(1)] = set(string_literals(match.group(2)))
    if not messages:
        raise Drift("match-signal-client.js accepts no service message shapes")
    return messages


def pinned_health(source: str) -> tuple[list[set[str]], set[str]]:
    """Reservation buckets and envelope keys the capacity gate pins exactly."""
    buckets: list[set[str]] = []
    cursor = 0
    while (position := source.find('"reservations": {', cursor)) >= 0:
        start = source.find("{", position + len('"reservations"'))
        buckets.append(set(re.findall(r'"(\w+)":', balanced(source, start))))
        cursor = start
    if not buckets:
        raise Drift("check_party_capacity.py pins no reservation buckets")
    envelope = re.search(r"health == \{", source)
    if envelope is None:
        raise Drift("check_party_capacity.py no longer pins the health body")
    return buckets, set(re.findall(
        r'^\s{28}"(\w+)":', balanced(source, envelope.end() - 1), re.MULTILINE))


# --- comparison --------------------------------------------------------------

def compare(role: str, schema: set[str], client: set[str],
            allowed_schema_only: frozenset[str] = frozenset(),
            consumer: str = "the browser client") -> list[str]:
    failures = []
    schema_only = schema - client
    client_only = client - schema
    for member in sorted(schema_only - allowed_schema_only):
        failures.append(f"{role}: the schema defines {member!r} and {consumer} "
                        "never reads it")
    for member in sorted(client_only):
        failures.append(f"{role}: {consumer} expects {member!r} and the schema "
                        "does not define it")
    for member in sorted(allowed_schema_only - schema_only):
        failures.append(f"{role}: {member!r} is allowlisted as a member "
                        f"{consumer} does not speak, but the sources no longer "
                        "disagree about it — update the allowlist")
    return failures


def analyze(sources: dict[str, str]) -> list[str]:
    schema = schema_vocabulary(sources)
    client = client_vocabulary(sources)
    failures: list[str] = []

    failures += compare("state body", schema["state"], client["state"])
    # Compared against WIRE_LOBBY_KEYS, the shape the client ACCEPTS, not
    # against LOBBY_KEYS, the canonical shape it projects. Those are different
    # decisions: refusing a field the service sends breaks the room outright,
    # while declining to project one is a presenter choice. Holding the client
    # to its projection here would report every unprojected field as drift and
    # make the two lists impossible to keep apart.
    failures += compare("lobby", schema["lobby"], client["wire_lobby"],
                        allowed_schema_only=R36_LOBBY_SCHEMA_ONLY)
    # Every canonical key must still be a key the client accepts, or the
    # presenter projects a field the wire check would have refused.
    unprojectable = client["lobby"] - client["wire_lobby"]
    if unprojectable:
        failures.append(
            f"lobby: LOBBY_KEYS projects {sorted(unprojectable)!r} which "
            "WIRE_LOBBY_KEYS does not accept")
    failures += compare("member", schema["member"], client["member"])
    failures += compare("seat", schema["seat"], client["seat"])
    failures += compare("control step", schema["control"], client["control"])
    failures += compare("compatibility", schema["compatibility"],
                        client["compatibility"])
    failures += compare("command request", schema["command_request"],
                        client["command_request"])
    failures += compare("lobby phase", schema["phases"], client["phases"])
    failures += compare("closed reason", schema["closed_reasons"],
                        client["closed_reasons"])

    if schema["schema_version"] != client["schema_version"]:
        failures.append(
            f"schemaVersion: the browser client pins "
            f"{client['schema_version']} and the schema stores "
            f"{schema['schema_version']}")

    accepted = client_accepted_envelopes(sources, client)
    for label, keys in schema_envelopes(sources, schema["state"]):
        stripped = keys - R36_ENVELOPE_SCHEMA_ONLY
        if stripped not in accepted:
            failures.append(
                f"{label}: the browser client admits no wire combination "
                f"matching {sorted(stripped)}")
    delivered = set().union(*(keys for _, keys in
                              schema_envelopes(sources, schema["state"])))
    for member in sorted(R36_ENVELOPE_SCHEMA_ONLY - delivered):
        failures.append(f"envelope: {member!r} is allowlisted as an R36 "
                        "response member the client refuses, but the Worker no "
                        "longer sends it — update the allowlist")

    unknown = client["commands"] - schema["commands"]
    for name in sorted(unknown):
        failures.append(f"command: the browser sends {name!r}, which "
                        "MATCH_COMMAND_TYPES does not define")
    unsent = schema["commands"] - client["commands"]
    for name in sorted(unsent - set(COMMANDS_NOT_SENT_BY_BROWSER)):
        failures.append(f"command: the schema defines {name!r} and no browser "
                        "path issues it")
    for name in sorted(set(COMMANDS_NOT_SENT_BY_BROWSER) - unsent):
        failures.append(f"command: {name!r} is allowlisted as browser-unsent, "
                        "but the sources no longer disagree about it — update "
                        "COMMANDS_NOT_SENT_BY_BROWSER")

    for name in sorted(client["command_result_reads"] - schema["control"]):
        failures.append(f"command result: the browser reads {name!r}, which "
                        "MatchStep does not carry")

    close_pairs = schema_close_reasons(sources["match_room"])
    terminal_codes = {code for code, _ in client["terminal_closes"]}
    for pair in sorted(client["terminal_closes"] - close_pairs):
        failures.append(f"socket close: the browser treats {pair} as terminal "
                        "and the room object never sends it")
    for code, reason in sorted(close_pairs - client["terminal_closes"]):
        if code in terminal_codes:
            failures.append(
                f"socket close: the room object sends ({code}, {reason!r}) and "
                "terminalLiveCloseCode does not name it, so the browser "
                "reconnects into a terminal room")
        elif code not in NON_TERMINAL_CLOSE_CODES:
            failures.append(
                f"socket close: ({code}, {reason!r}) is neither a browser "
                "terminal verdict nor an allowlisted retryable close")

    tokens = schema_error_tokens(sources, schema["errors"],
                                 {reason for _, reason in close_pairs})
    for code in sorted(client["recovery_codes"] - tokens -
                       CLIENT_ONLY_RECOVERY_CODES):
        failures.append(f"recovery: failureSlug branches on {code!r}, which no "
                        "service error body, MatchError or close reason emits")

    sent, envelope = client_signal_messages(sources["signal_client"])
    accepted_signals = schema_signal_messages(sources["signaling"])
    for name in sorted(set(sent) ^ set(accepted_signals)):
        failures.append(f"signaling: message type {name!r} exists on only one "
                        "side of the wire")
    for name in sorted(set(sent) & set(accepted_signals)):
        failures += compare(f"signaling {name}", accepted_signals[name],
                            sent[name])
    failures += compare("signaling envelope",
                        set(named_array(sources["signaling"], "BASE_KEYS",
                                        "signaling")), envelope)

    pushed = schema_signal_service_messages(sources["match_room"])
    parsed = client_signal_service_messages(sources["signal_client"])
    for name in sorted(set(pushed) - set(parsed)):
        failures.append(f"signaling: the room object pushes {name!r} and the "
                        "browser client rejects it")
    for name in sorted(set(pushed) & set(parsed)):
        failures += compare(f"signaling push {name}", pushed[name], parsed[name])

    buckets, health_envelope = schema_health(sources)
    pinned_buckets, pinned_envelope = pinned_health(sources["capacity_gate"])
    for index, pinned_bucket in enumerate(pinned_buckets):
        failures += compare(f"/api/ops/health reservations pin {index}",
                            buckets, pinned_bucket,
                            consumer="the capacity gate's exact pin")
    failures += compare("/api/ops/health body", health_envelope, pinned_envelope,
                        consumer="the capacity gate's exact pin")
    return failures


# --- positive controls -------------------------------------------------------

CONTROLS = (
    # With R36 closed there is no allowlist to land in: renaming a lobby field
    # in the schema must now read as ordinary drift against a client that
    # accepts the real name.
    ("protocol", "  points: number[];", "  scores: number[];",
     "the schema defines 'scores'"),
    ("protocol", "  localIndex: number;", "  localIndex: number;\n  nickname: string;",
     "the schema defines 'nickname'"),
    ("protocol", '| "set_cup";', '| "set_cup" | "set_teleport";',
     "MATCH_COMMAND_TYPES and MatchCommandType disagree"),
    ("protocol", '"set_cup",\n]', '"set_cup", "set_teleport",\n]',
     "MATCH_COMMAND_TYPES and MatchCommandType disagree"),
    ("live_state", '"localIndex", "characterId", "vehicleId"]',
     '"localIndex", "characterId", "vehicleId", "nickname"]',
     "the browser client expects 'nickname'"),
    ("live_state", "MATCH_STATE_SCHEMA_VERSION = 2",
     "MATCH_STATE_SCHEMA_VERSION = 3",
     "the browser client pins 3"),
    ("live_state", '"host_closed", "room_expired"].includes',
     '"host_closed", "room_gone"].includes',
     "closed reason"),
    ("match_room", 'closeSocket(socket, 4000, "host_closed")',
     'closeSocket(socket, 4000, "host_ended")',
     "terminalLiveCloseCode does not name it"),
    ("match_room", "expiresAt: record.expiresAt,", "",
     "admits no wire combination"),
    ("signaling", '"candidate", "sdpMid", "sdpMLineIndex", "usernameFragment"',
     '"candidate", "sdpMediaId", "sdpMLineIndex", "usernameFragment"',
     "signaling webrtc_ice"),
    ("budget", '"partyNativeCreate", "turnMint",',
     '"partyNativeCreate", "turnMint", "turnRefresh",',
     "/api/ops/health reservations pin"),
    ("room", "expectedRevision: liveRoom.lobby.revision",
     "revision: liveRoom.lobby.revision",
     "command request"),
    ("room", 'sendLiveCommand("set_vote"', 'sendLiveCommand("set_warp"',
     "the browser sends 'set_warp'"),
    ("room", 'sendLiveCommand("set_vote"', 'sendLiveCommand("set_cup"',
     "'set_cup' is allowlisted as browser-unsent"),
    # Was: prove the gate notices when the gap closes. R36 is closed, so the
    # same mutation now proves the plain equality check still bites in the
    # other direction -- the schema moving away from the client.
    ("protocol", "  schemaVersion: 2;", "  schemaVersion: 1;",
     "the browser client pins 2 and the schema stores 1"),
)


def run_controls(sources: dict[str, str]) -> None:
    """Every control must break the parity it targets, both directions."""
    for index, (name, before, after, expected) in enumerate(CONTROLS):
        if sources[name].count(before) < 1:
            raise Drift(f"positive control {index} cannot be applied: "
                        f"{before[:50]!r} is absent from {name}")
        mutated = dict(sources)
        mutated[name] = sources[name].replace(before, after, 1)
        try:
            failures = analyze(mutated)
        except Drift as error:
            failures = [str(error)]
        if not any(expected in failure for failure in failures):
            raise Drift(f"positive control {index} ({name}: {before[:40]!r}) "
                        f"did not produce {expected!r}; got {failures}")
    for name in ("protocol", "live_state", "signaling"):
        blanked = dict(sources)
        blanked[name] = ""
        try:
            analyze(blanked)
        except Drift:
            continue
        raise Drift(f"an empty {name} source did not fail the gate closed")


def main() -> int:
    sources = {}
    for name, path in SOURCE_FILES.items():
        if not path.is_file():
            raise Drift(f"{path.relative_to(ROOT)} has moved or been deleted")
        text = path.read_text(encoding="utf-8")
        sources[name] = text if name == "capacity_gate" else scrub(text)

    failures = analyze(sources)
    run_controls(sources)
    if failures:
        raise Drift("online wire schema parity:\n  " + "\n  ".join(failures))

    schema = schema_vocabulary(sources)
    client = client_vocabulary(sources)
    schema_size = sum(len(schema[role]) for role in
                      ("state", "lobby", "member", "seat", "control",
                       "compatibility", "command_request", "commands",
                       "phases", "errors", "closed_reasons"))
    client_size = sum(len(client[role]) for role in
                      ("state", "identity", "invite", "lobby", "member", "seat",
                       "control", "compatibility", "command_request",
                       "commands", "phases", "closed_reasons"))
    print(f"check_online_wire_schema_parity: PASS — {schema_size} schema and "
          f"{client_size} client wire members agree across state, command, "
          f"close, error, signaling and /api/ops/health vocabularies; "
          f"{len(R36_LOBBY_SCHEMA_ONLY) + len(R36_ENVELOPE_SCHEMA_ONLY) + 1} "
          f"R36 members and {len(COMMANDS_NOT_SENT_BY_BROWSER)} browser-unsent "
          f"commands allowlisted; {len(CONTROLS)} positive controls rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
