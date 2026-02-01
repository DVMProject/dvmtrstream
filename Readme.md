# DVM-TR-Stream

Send audio from trunk-recorder to dvmbridge!

## Example Configuration JSON

The configuration for the dvmtrstream plugin, is extremely similar to the simplestreamer plugin. See below.

- interCallDelay - This configures a delay in-between individual call streams in milliseconds.
- silenceLeader - This configures an injected silence leader before actual call stream audio.

```
{
    ...
    "sources": [ ... ],
    "systems": [ ... ],
    "plugins": [{
        "library": "libdvmtrstream.so",
        "interCallDelay": 50,
        "silenceLeader": 120,
        "streams": [
            { "TGID": 1, "shortName": "SystemName", "address": "127.0.0.1", "port": 32001 },
            { "TGID": 2, "shortName": "SystemName", "address": "127.0.0.1", "port": 32002 },
            { "TGID": 3, "shortName": "SystemName", "address": "127.0.0.1", "port": 32003 }
        ]
    }]
}
```

## Building

To use dvmtrstream with trunk-recorder, clone this repository into the trunk-recorder `user_plugins` folder and then build trunk-recorder.

```
cd trunk-recorder/user_plugins
git clone https://github.com/DVMProject/dvmtrstream.git
```

For more information, see https://github.com/TrunkRecorder/trunk-recorder/blob/master/docs/Plugins.md