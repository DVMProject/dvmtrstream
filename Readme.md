# DVM-TR-Stream

Send audio from trunk-recorder to dvmbridge!

## Example Configuration JSON

The configuration for the dvmtrstream plugin, is extremely similar to the simplestreamer plugin. See below.

- interCallDelay - This configures a delay in-between individual call streams in milliseconds.
- silenceLeader - This configures an injected silence leader before actual call stream audio.
- strictCallSerialization - When true (default), keep one stream pinned per endpoint and only switch after an idle gap.
- callGapHoldMs - Idle gap window used by strict serialization before allowing stream switch (default: 250 ms).
- sendTickMs - Scheduler tick interval for endpoint send loop (default: 15 ms).
- callActiveStaleMs - Failsafe timeout to release a pinned active-call latch if no audio activity arrives (default: 1200 ms).

```
{
    ...
    "sources": [ ... ],
    "systems": [ ... ],
    "plugins": [{
        "library": "libdvmtrstream.so",
        "interCallDelay": 50,
        "silenceLeader": 120,
        "strictCallSerialization": true,
        "callGapHoldMs": 250,
        "sendTickMs": 15,
        "callActiveStaleMs": 1200,
        "streams": [
            { "TGID": 1, "shortName": "SystemName", "address": "127.0.0.1", "port": 32001 },
            { "TGID": 2, "shortName": "SystemName", "address": "127.0.0.1", "port": 32002 },
            { "TGID": 3, "shortName": "SystemName", "address": "127.0.0.1", "port": 32003 }
        ]
    }]
}
```

## Muxing Multiple TGIDs To One UDP Endpoint

When multiple stream entries point to the same `address:port`, dvmtrstream will mux them into one endpoint send loop.

To enforce strict call-serialized behavior (avoid interleaving during short callback gaps):

- set `strictCallSerialization: true`
- start with `callGapHoldMs: 250` (use `300-500` if your system has larger callback jitter)
- keep `sendTickMs: 15` to match 20 ms audio chunk pacing
- set `callActiveStaleMs: 1200` (raise to `2000-3000` only if your call metadata updates are delayed)

Example (shared endpoints):

```
"streams": [
    { "TGID": 3111, "shortName": "SuffolkP25", "address": "10.7.4.198", "port": 32401 },
    { "TGID": 3211, "shortName": "SuffolkP25", "address": "10.7.4.198", "port": 32401 },
    { "TGID": 3311, "shortName": "SuffolkP25", "address": "10.7.4.198", "port": 32402 },
    { "TGID": 3411, "shortName": "SuffolkP25", "address": "10.7.4.198", "port": 32402 }
]
```

Notes:

- `interCallDelay` adds extra delay after a stream is considered complete. It is optional when strict serialization is enabled.
- Larger `callGapHoldMs` reduces stream switching during jitter but can increase handoff latency to the next queued stream.
- `callActiveStaleMs` is a failsafe release timer. It prevents permanent latch if call end metadata is delayed or missing.

## Building

To use dvmtrstream with trunk-recorder, clone this repository into the trunk-recorder `user_plugins` folder and then build trunk-recorder.

```
cd trunk-recorder/user_plugins
git clone https://github.com/DVMProject/dvmtrstream.git
```

For more information, see https://github.com/TrunkRecorder/trunk-recorder/blob/master/docs/Plugins.md

## License

This project is licensed under the GPLv2 License - see the [LICENSE](LICENSE) file for details. Use of this project is intended, for amateur and/or educational use ONLY. Any other use is at the risk of user and all commercial purposes is strictly discouraged.

**THIS SOFTWARE MUST NEVER BE USED IN PUBLIC SAFETY OR LIFE SAFETY CRITICAL APPLICATIONS! This software project is provided solely for personal, non-commercial, hobbyist use; any commercial, professional, governmental, or other non-hobbyist use is strictly discouraged, fully unsupported and expressly disclaimed by the authors.**

By using this software, you agree to indemnify, defend, and hold harmless the authors, contributors, and affiliated  parties from and against any and all claims, liabilities, damages, losses, or expenses (including reasonable  attorneys’ fees) arising out of or relating to any unlawful, unauthorized, or improper use of the software.

