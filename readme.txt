Shroudtopia

Modloader for Enshrouded (Server & Client))



I'm excited to introduce **Shroudtopia**, a modloader that allows easy management and integration of mods for Enshrouded. With Shroudtopia, you can dynamically load, activate, and deactivate mods without restarting, giving you the ultimate flexibility to enhance gameplay.


## Project status and links

This repository preserves the Shroudtopia compatibility layer for Shroudweave. Changes to the upstream layer here are maintenance-only. Shroudtopia is the MIT-licensed upstream project by Miguel Oppermann (s0t7x): https://github.com/s0t7x/shroudtopia.

Current public Shroudweave Loader release: v0.2.1-shroudweave.1 - https://github.com/Aerox912/shroudtopia/releases/tag/v0.2.1-shroudweave.1
Public Shroudweave Loader listing: https://www.nexusmods.com/enshrouded/mods/216

## Modloader Features

- **Mod Management**: Dynamically load and unload mods from the "mods" folder.
- **Live Configuration**: Modify mod settings at runtime without restarting the server.
- **Dependency Injection**: Each mod is fully integrated into the system via the `ModContext`, enabling shared access to configuration, logging, and other utilities.


## Example Mods

- **Flight Mod:** Client-side free glider flight. Dedicated-server flight is not supported by this client DLL alone. Real-game flight and popup acceptance remains required before release. Documentation: example-mods/flight_mod/README.md

- **First Person View:** Client-side first-person camera mod. Documentation: example-mods/first_person_view/README.md

## Installation

1. **Download:** Get the current public Shroudweave Loader release: https://github.com/Aerox912/shroudtopia/releases/tag/v0.2.1-shroudweave.1
2. **Setup:** Extract the files into your Enshrouded game or dedicated server folder.
3. **Mods Folder:** Create a `mods` folder if it doesn’t exist and place your mod DLLs inside.
4. **Launch:** Start the server - a default config is generated if `shroudtopia.json` is absent.

If Shroudtopia is loaded correctly, you should see something like this in the server console:
```
[shroudtopia][INFO] Config loaded.
[shroudtopia][INFO] Wait before injection. Configured boot delay is 3000ms.
```

Upon the first launch, a default configuration file `shroudtopia.json` is created. All mods are deactivated by default, so you must manually activate them by adjusting the configuration.


## Configuration

Each mod can be enabled or customized via the shroudtopia.json config file. This example shows the retained module keys; both modules start disabled here.

```json
{
    "active": true,
    "bootDelay": 3000,
    "enableLogging": true,
    "logLevel": "INFO",
    "mods": {
        "Flight Mod": {
            "active": false,
            "toggle_key": 117
        },
        "FirstPersonView": {
            "active": false,
            "camera_distance": -0.15
        }
    },
    "updateDelay": 500
}
```

Flight toggle_key defaults to 117 (F6); 0 disables the key. Flight remains client-side, and real-game flight and popup acceptance is still required before release. See example-mods/flight_mod/README.md.
First Person View camera_distance defaults to -0.15 on the supported current Steam client. See example-mods/first_person_view/README.md for supported client details.

# Creating Mods

Mods for Shroudtopia are written as dynamic libraries (DLLs) and include `shroudtopia.h` from the `include/` directory. Each mod implements the Mod interface and exports the factory function extern "C" __declspec(dllexport) Mod* CreateModInstance().

The retained First Person View example defines FirstPersonMod and exports this factory from example-mods/first_person_view/dllmain.cpp:
```cpp
extern "C" __declspec(dllexport) Mod* CreateModInstance()
{
    return new FirstPersonMod();
}
```

See example-mods/first_person_view/README.md and its source for the complete implementation.

## Mod Interface
Every mod must implement the following functions to integrate with the modloader:
```cpp
class Mod {
public:
    virtual ~Mod() {}
    virtual ModMetaData GetMetaData() = 0;
    virtual void Load(ModContext* modContext) = 0;
    virtual void Unload(ModContext* modContext) = 0;
    virtual void Activate(ModContext* modContext) = 0;
    virtual void Deactivate(ModContext* modContext) = 0;
    virtual void Update(ModContext* modContext) = 0;
};
```

`ModMetaData` contains essential information about the mod:
```cpp
struct ModMetaData {
    std::string name;
    std::string description;
    std::string version;
    std::string author;
    std::string targetShroudtopiaVersion;
    bool hasClientSupport;
    bool hasServerSupport;
};
```

## ModContext
The `ModContext` is passed to each mod and provides access to core functionality, such as configuration, logging, and mod management. For example, the mod can retrieve config values via:
```cpp
bool enabled = modContext->config.GetBool("modName", "feature_name", false);
```


# Contributing

Changes to the Shroudtopia compatibility layer are maintenance-only. Preserve the upstream MIT license and notices, keep compatibility contracts explicit, and document retained modules with accurate support and acceptance details.

# License
This project is licensed under the [MIT License](https://github.com/s0t7x/shroudtopia/blob/0.1-stable/LICENSE).

<hr />

Let's make Enshrouded even more exciting with **Shroudtopia**! 🌟

Special thanks to the following folks. Their work gave me a headstart implementing most features:

- cfe
- Turk
- Atamg