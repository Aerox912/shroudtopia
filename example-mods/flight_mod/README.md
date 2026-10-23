# Always Flying toggle

Press **F6** while playing to switch between free glider flight and ordinary gliding. A brief On/Off popup confirms the change. Flight starts off when the mod activates. Open a glider using the game's normal controls; this mod changes its descent limit, not the controls used to deploy it.

Set `mods["Flight Mod"].toggle_key` in `shroudtopia.json` to a Windows virtual-key number to change the key (117 is F6; 0 disables the key). The manager exposes this under Settings → Hotkeys. Held keys and focus changes do not trigger repeated toggles.

Requires Shroudtopia and the Windows Steam client data revision 1076226. The executable layout and original instructions are checked before activation. Unsupported builds retain vanilla flight. This is a client mod; do not install it on a dedicated server.

Dedicated-server flight is not currently supported by this client DLL alone. On 7 October, a player reported continued descent with the glider open and Flight On, including backward controller input. Static inspection found the matching descent clamp in the server executable. A server companion with authenticated player ownership and synchronized activation is still in development; the On popup currently confirms only the local toggle.

For development output, `flight_mod.vcxproj` writes the Flight DLL to `$(SolutionDir)/<platform>/<configuration>/mods/`. This places the DLL directly in the top-level `mods` directory used by the loader legacy DLL scan. The example has no `mod.json`, so a nested `mods/flight_mod/` directory alone is not discovered. Never load this and a second Flight Mod DLL together.

Based on s0T7x / Miguel Oppermann's original MIT-licensed Flight Mod in [Shroudtopia](https://github.com/s0t7x/shroudtopia). Aerox912 maintains the build guard, toggle and feedback. [Always Flying on Nexus](https://www.nexusmods.com/enshrouded/mods/118), uploaded by Tazz, is the original mod listing that prompted this integration. This package is built from Shroudtopia source and does not contain that upload's binary. Enshrouded belongs to Keen Games.

Offline checks exercise the detour, restoration, concurrent value updates and keyboard state. Real-game flight and popup acceptance remains required before release.
