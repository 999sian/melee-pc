# DHT dependency provenance

Downloaded 2026-09-20 from:

- https://github.com/jech/dht (`master`, `dht.c`, `dht.h`), MIT license retained in both files.
- https://github.com/clibs/sha1 (`master`, `sha1.c`, `sha1.h`), Steve Reid public-domain SHA-1, notice retained.

Local patches: outgoing `announce_peer` includes canonical `implied_port=1`;
SHA1 endian preprocessor checks require defined macros before comparing them.

`src/pc/net_dht.c` owns the IPv4 nonblocking UDP socket until `take_socket`.
It passes public DHT packets to jech/dht and non-DHT packets to the pairing
callback; discovery, simultaneous open, and game traffic retain one NAT mapping.
DNS bootstrap resolves off the game thread. Up to 64 compact node endpoints are
cached in SDL's `melee-pc` preference directory as `dht-nodes-v1` and pinged on
restart. Search waits for four good nodes and thirty good/dubious nodes.

External-endpoint observation uses a separate bounded bencode parser, accepts
only top-level compact `ip` values in responses matched to outgoing transactions,
and requires agreement from three distinct /24 source networks. Votes expire
in ten minutes. This is an observation heuristic, not cryptographic authentication.

This module implements BEP5 rendezvous, `implied_port`, direct/unranked/ranked
time topics, and endpoint observation. `net_dht_item.c` adds client-only BEP44
mutable and immutable byte-string get/put: bounded iterative XOR-nearest lookup, four get
requests in flight, a 32-node shortlist, at most 64 get requests, and writes to
the closest eight token-bearing respondents. Responses must match the request's
random 64-bit transaction and endpoint. Signatures use canonical salt/sequence/
value bytes and the existing Ed25519 identity helpers. Sequence conflicts and
replays and signed same-sequence conflicts are rejected; mutable writes carry CAS when an existing authenticated sequence
was observed. Encoded values obey the BEP44 1000-byte limit (996 payload bytes),
salts are limited to 64 bytes, and timeouts bound the operation.

Public DHT nodes host mutable values; this client does not serve mutable storage.
The caller must persist its sequence and periodically republish records (BEP44
recommends hourly; values can expire after two hours). A positive callback reports
actual responding replicas, not guaranteed global availability. There is no relay
traversal or IPv6 DHT. Real Internet/NAT interoperability still requires testing
between different networks.

Offline tests:

- `tools/test_net_dht.c`: standard SHA1/topic vectors and socket ownership/handoff.
  Compile with `PC_DHT_TEST_NO_BOOTSTRAP`; the test uses loopback UDP only.
- `tools/test_net_dht_observation.c`: bounded bencode parsing, malformed/truncated
  packets, unsolicited responses, duplicate source-network votes, consensus/expiry.
  This test includes `net_dht.c` for internal parser access.
- `tools/test_net_dht_item.c`: a deterministic fake transport, both official BEP44
  signature vectors, canonical signed output, salt/size limits, authentic/forged
  records, tokens/CAS, iterative lookup, spoof rejection, conflicts, timeout/cancel.
  This test includes `net_dht_item.c` and supplies transport and SHA1 adapters.

Immutable targets use the standard SHA1(bencoded value) locator; their GET values
are hash-checked, and their PUT requests omit mutable key/signature/sequence fields.
The caller verifies any application signatures inside immutable records.

The item test links `src/pc/net_identity.c`, `extern/dht/sha1.c`, both Monocypher
sources, and SDL3. The observation test additionally links `net_dht_item.c` and
`extern/dht/dht.c`; do not separately compile the source included by either test.
Production modules compile with GCC and MinGW `-Wall -Wextra -Werror`. Item and
observation tests pass AddressSanitizer/UndefinedBehaviorSanitizer (LeakSanitizer
is unavailable under the task runner's ptrace sandbox).

Public-network verification (2026-09-20, explicitly authorized): a temporary
random-key probe bootstrapped the public routing table, published a 36-byte
BEP44 value with salt `probe` at sequence 1, received five positive storage
acknowledgements, then independently looked up and authenticated the identical
value/sequence using the production client. The entire probe completed in about
62 seconds. This verifies public DHT storage/retrieval from one host; it does not
establish two-NAT gameplay interoperability or long-term availability.

A second authorized probe published a unique 69-byte immutable value, received
six positive storage acknowledgements, then retrieved the exact bytes by their
BEP44 SHA1 target using the production client. Both operations returned success
with sequence zero, and the probe completed in about 62 seconds. This confirms
public immutable storage/retrieval from the same host, with the same limitations.
