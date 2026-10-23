<p align="center">
<img alt="Shroudtopia" src="https://github.com/s0t7x/shroudtopia/blob/main/logo_crop.png">
</p>
<p align="center">
<img alt="Static Badge" src="https://img.shields.io/badge/Game%20Version%20(SVN)-645934-blue">
<img alt="Static Badge" src="https://img.shields.io/badge/Core%20Version-0.0.3-blue">
</p>
<p align="center">
<table class="n" border="0" align="center">
 <tr>
<td>- <a href="#modloader-features">Modloader Features</a></td>

 </tr>
 <tr>
<td>- <a href="#installation">Installation</a></td>

 </tr>
 <tr>
<td>- <a href="#configuration">Configuration</a></td>

 </tr>
 <tr>
<td>- <a href="#roadmap">Roadmap</a></td>
 </tr>
 <tr>
<td>- <a href="#example-mods">Example Mods</a></td>
 </tr>
     <tr>
<td>- <a href="#creating-mods">Creating Mods</a></td>
 </tr>
     <tr>
     <td>- <a href="#contributing">Contributing</a></td>
 </tr>
     <tr>
     <td>- <a href="#license">License</a></td>
 </tr>
</table>
</p>

## Project status and links

This repository preserves the Shroudtopia compatibility layer for Shroudweave. Changes to the upstream layer here are maintenance-only. Shroudtopia is the MIT-licensed upstream project by Miguel Oppermann (s0t7x): [source repository](https://github.com/s0t7x/shroudtopia).

Current public Shroudweave Loader release: [v0.2.1-shroudweave.1](https://github.com/Aerox912/shroudtopia/releases/tag/v0.2.1-shroudweave.1). Public Shroudweave Loader listing: [Nexus Mods](https://www.nexusmods.com/enshrouded/mods/216).

## Modloader Features

- **Mod Management**: Dynamically load and unload mods from the "mods" folder.
- **Live Configuration**: Modify mod settings at runtime without restarting the server.
- **Dependency Injection**: Each mod is fully integrated into the system via the `ModContext`, enabling shared access to configuration, logging, and other utilities.

## Installation

### Requirements
- [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe)

### Steps to Install:
1. **Download the Mod Loader:** Download the current [public Shroudweave Loader release](https://github.com/Aerox912/shroudtopia/releases/tag/v0.2.1-shroudweave.1).
2. **Extract the Files:** Extract the contents into your Enshrouded game or dedicated server folder.
3. **Choose an example mod:** See the [Flight Mod](example-mods/flight_mod/README.md) and [First Person View](example-mods/first_person_view/README.md) documentation for the retained modules and their requirements.
4. **Create Mods Folder:** If it doesn’t already exist, create a `mods` folder and place your mod DLLs inside.
5. **Launch the Server:** Start the server. A default config (`shroudtopia.json`) will be generated if it's absent.

Once set up, you're ready to start using Shroudtopia to manage your mods.

On servers: If Shroudtopia is loaded correctly, you should see something like this in the server console:
```
[shroudtopia][INFO] Config loaded.
[shroudtopia][INFO] Wait before injection. Configured boot delay is 3000ms.
```

Upon the first launch, a default configuration file `shroudtopia.json` is created. For mods you must manually adjust the configuration to their needs.

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

Mods are free to expose configuration options.

## Roadmap

<p align="center">
<img alt="Flight Mod" src="https://github.com/s0t7x/shroudtopia/blob/main/roadmap_2025.png">
</p>

## Example Mods

These are the retained examples in this checkout. Follow their module documentation for configuration and compatibility details.

### Flight Mod

The client module toggles free glider flight. Dedicated-server flight is not supported by this client DLL alone. Real-game flight and popup acceptance remains required before release. See the [Flight Mod README](example-mods/flight_mod/README.md).

### First Person View

The client module restores the first-person camera on supported layouts. See the [First Person View README](example-mods/first_person_view/README.md) for supported client details and configuration.

# Creating Mods

Mods for Shroudtopia are written as dynamic libraries (DLLs) and include `shroudtopia.h` from the `include/` directory. Each mod implements the Mod interface and exports the factory function extern "C" __declspec(dllexport) Mod* CreateModInstance().

The retained First Person View example defines FirstPersonMod and exports this factory from example-mods/first_person_view/dllmain.cpp:
```cpp
extern "C" __declspec(dllexport) Mod* CreateModInstance()
{
    return new FirstPersonMod();
}
```

See the [First Person View README](example-mods/first_person_view/README.md) and source for the complete implementation.

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

<a href="https://www.dmca.com/Protection/Status.aspx?id=f9dcfb4f-6a12-4640-9ae3-e2b3e5ae4c7b&refurl=https%3a%2f%2fgithub.com%2fs0t7x%2fshroudtopia&rlo=true">
<img alt="Shroudtopia" src="https://github.com/user-attachments/assets/11ab9970-c8b3-4137-99d6-7a9892459d02">
</a>
<hr />

Let's make Enshrouded even more exciting with **Shroudtopia**! 🌟

Special thanks to the following folks. Their work gave me a headstart implementing most features:

- cfemen
- Turk
- Atamg
- ndoa
