# Odyssey Mod Switch

A Nintendo Switch homebrew app for turning Super Mario Odyssey mods on and off without a PC or a reboot. It works like Minecraft's resource pack screen: press A or tap a mod to move it from **Available** to **Selected**. Only one mod can be selected at a time, because these mods replace the same game files.

Changes apply the next time you launch the game.

## Setup

1. Put `OdysseyModSwitch.nro` in `/switch/` on your SD card.
2. Put each mod in its own folder:

   ```
   /mods/Super Mario Odyssey/<Mod Name>/atmosphere/contents/0100000000010000/...
   /mods/Super Mario Odyssey/<Mod Name>/atmosphere/exefs_patches/StarlightBase/...
   ```

   Mods aren't included in this repo. Get them from their creators.

3. Open the app from the Homebrew Menu, with the game closed.

## Safety

- **The library is read-only.** Your mod folders are only read. The one exception is adding a `_Recovered-N` folder.
- **Deletes stay in one place.** The app only deletes inside `/config/odyssey-mod-switch`. Active files are moved there first.
- **Unknown files are kept.** Active files that don't match a mod in the library, such as a hand-edited mod, move to `_Recovered-N` instead of being deleted.
- **Copies are verified first.** A mod is copied and compared byte for byte before the running mod is touched, so a failed copy leaves the current mod in place.
- **Cheats are preserved.** A `cheats` folder in the game's contents folder survives every switch.
- **Mods can't reach elsewhere.** A mod with files outside those two folders is marked unsupported and can't be turned on.
- **Mods only turn on in emuMMC.** On sysCFW, or if the system can't be identified, turning a mod on is refused. Turning mods off always works.
- **Running games are protected.** Switching is refused while a game is running.

There's one gap: a mod left on in emuMMC stays on when you boot sysCFW, because both use the same folder. Turn mods off before leaving emuMMC, or don't launch the game on sysCFW.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`. The build scripts don't support spaces in the project path.

```bash
export DEVKITPRO=/opt/devkitpro
make
```

## Testing

The core logic in `source/modswitch.c` has no Switch dependencies, so it can be tested on a computer against a fake SD card. The suite simulates a power cut and an I/O error at every file change, and double power cuts during recovery.

```bash
clang -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -DMS_TEST -g -O1 \
  -fsanitize=undefined -fno-sanitize-recover=all -fstack-protector-all \
  -o /tmp/ms_cli source/modswitch.c tests/cli.c
python3 tests/run_tests.py /tmp/ms_cli <mods-dir> <empty-work-dir>
```

`<mods-dir>` holds one folder per mod, each containing an `atmosphere` folder. The test script's `REAL` table lists the folder names it expects.

These tests don't cover the menu, or the Switch's own SD card behavior. It's been run on one console: an Erista on firmware 23.0.1 with Atmosphère 1.12.1.

## Credits

Built with [libnx](https://github.com/switchbrew/libnx). Thanks to the creators of the mods it was built around, including [CraftyBoss](https://github.com/CraftyBoss) and [Amethyst-szs](https://github.com/Amethyst-szs).
