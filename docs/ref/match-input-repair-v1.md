# Match input repair v1

Status: local foundation; production online-race admission remains disabled.

Explicit repair for an input gap the realtime carrier's redundancy has
outlived. The launcher owns it end to end: repaired input reaches the game
through the same `MdkrMatchTransport` ingress every other input does, under the
same authenticated slot mask, so a repair can authorize nothing a bundle could
not.

## Why a repair exists

`gb-match-state-v1` is `maxRetransmits 0`: a dropped datagram is gone. Each
64-byte bundle (`docs/ref/match-peer-carrier-v1.md`, payload type `0`) carries
three consecutive authored ticks, so a burst that loses fewer than three
consecutive sends is covered by the next bundle that lands. A longer burst
leaves a contiguous run of ticks that no later bundle will ever carry again.

The drain predicts through that run. Once the run's first tick falls further
behind the drain frontier than the retained rollback depth
(`MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS`), the timeline can no longer be
reconciled: `MdkrMatchTransport` latches `MDKR_MATCH_RECOVERY_INPUT_GAP` and
the race is over. Repair closes the run before that point.

The in-race resend sweep is not a substitute. It re-fans the author's trailing
window blindly on the same lossy channel; it does not know which ticks are
missing, and its retransmits are lost by the same burst that lost the originals.

## Lane

Repair rides `gb-match-authority-v1` — reliable, unordered, with its own
derived key, sequence space and replay window (see the carrier spec's channel
table). Reliable because a lost repair leaves the gap it names unfilled;
unordered because every message names the tick run it covers and is useful the
moment it lands.

## Messages

Both kinds are exactly 64 bytes, the size every sealed peer payload already is,
carried as authenticated payload type `2` (request) and `3` (answer). All
integers are big-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 2 | `MR` |
| 2 | 1 | version `1` |
| 3 | 1 | kind: request `0`, answer `1` |
| 4 | 1 | answer: canonical slot `0`–`3`; request: zero |
| 5 | 1 | count: ticks named by this message |
| 6 | 4 | match epoch (nonzero) |
| 10 | 4 | first tick of the contiguous run |
| 14 | 48 | answer: twelve 4-byte pad samples; request: zero |
| 62 | 2 | CRC-16/CCITT over bytes 0–61 |

Each pad sample is `buttons` (2 bytes), `stick_x` and `stick_y` (1 signed byte
each). A carried sample must be present with both sticks within ±80, exactly
the shape the bundle carrier holds. Cells past `count` are zero on the wire, so
two encodings of one run are byte-identical.

## Batch cap

A request may name at most `MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS` ticks. Beyond
that the run could not be reconciled even if it arrived, so asking would only
spend bandwidth on input the rollback window has already passed.

One answer message carries twelve ticks for one canonical slot, so a request
costs at most three answer messages per slot the responder owns — at most
twelve messages against the maximum four-seat roster. The responder answers for
every slot it owns; the requester does not choose, and a request naming a slot
is malformed.

## Epoch scoping

Both messages carry the match epoch, and a receiver drops any repair whose
epoch is not its current one. Ticks are epoch-relative, so a repair minted
during a retired epoch names ticks that mean something else in the next one; it
is dropped at the boundary rather than carried across it.

## Determinism

A repaired input is the same input. An answer's frames come from the author's
own committed local input — the identical function its own drain and every
bundle retransmit read — and the requester admits them through the identical
`mdkr_match_transport_receive` path a bundle takes. An author never answers a
tick it has not itself committed to, so no peer can commit a frame the author
will later contradict. Both endpoints therefore commit byte-identical canonical
input for a repaired tick, and the live lanes assert exactly that.

## Requesting policy

One request per remote canonical slot per gap. A gap is worth asking about once
its first tick is more than three authored ticks behind the drain frontier —
the point past which the carrier's redundancy can no longer cover it. Because
the lane is reliable, a sent request is delivered; re-asking for a first tick
already asked for would only duplicate the answer. A gap whose first tick moves
is a different gap and is asked for again. Requests and answers are recorded in
the forensics ring under the input-prediction record, which is exactly the
condition that raises them: a repair record names a canonical slot, its detail
is the repair kind and its two values are the run, while the transport's own
prediction record carries no slot and the committed masks.
