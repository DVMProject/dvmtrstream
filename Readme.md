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

## License

This project is licensed under the GPLv2 License - see the [LICENSE](LICENSE) file for details. Use of this project is intended, for amateur and/or educational use ONLY. Any other use is at the risk of user and all commercial purposes is strictly discouraged.

**THIS SOFTWARE MUST NEVER BE USED IN PUBLIC SAFETY OR LIFE SAFETY CRITICAL APPLICATIONS! This software project is provided solely for personal, non-commercial, hobbyist use; any commercial, professional, governmental, or other non-hobbyist use is strictly discouraged, fully unsupported and expressly disclaimed by the authors.**

By using this software, you agree to indemnify, defend, and hold harmless the authors, contributors, and affiliated  parties from and against any and all claims, liabilities, damages, losses, or expenses (including reasonable  attorneys’ fees) arising out of or relating to any unlawful, unauthorized, or improper use of the software.

