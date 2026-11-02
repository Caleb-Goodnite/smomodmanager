#!/usr/bin/env python3
"""Tests the mod switcher against a fake SD card, using the three real mods.

Usage: run_tests.py CLI_BINARY REAL_MODS_DIR WORK_DIR
"""
import hashlib
import os
import shutil
import subprocess
import sys

CLI, REAL_MODS, WORK = sys.argv[1], sys.argv[2], sys.argv[3]

LIB = "mods/Super Mario Odyssey"
ACTIVE_T = "atmosphere/contents/0100000000010000"
ACTIVE_P = "atmosphere/exefs_patches/StarlightBase"
WORKDIR = "config/odyssey-mod-switch"
REAL = {
    "SMO Online": "SMO_Online_v1.4.0_for_Switch",
    "Time Travel": "time-travel-build-100",
    "Shadow Clones": "shadow-clones-smo-V11",
}
IGNORED = {".DS_Store", "romfs_metadata.bin", "cheats"}

passed = 0
failures = []


def check(condition, label):
    global passed
    if condition:
        passed += 1
    else:
        failures.append(label)
        print("FAIL:", label)


def write(root, relative, data=b"x"):
    path = os.path.join(root, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


def fresh(real=True):
    root = os.path.join(WORK, "sd")
    if os.path.lexists(root):
        subprocess.run(["chmod", "-R", "u+rwx", root], check=False)
        shutil.rmtree(root)
    os.makedirs(root)
    protected = {
        "atmosphere/contents/420000000000000B/exefs.nsp": b"sys-patch" * 500,
        "atmosphere/contents/420000000000000B/flags/boot2.flag": b"",
        "atmosphere/contents/0100000000020000/romfs/other_game.bin": b"other game mod",
        "atmosphere/exefs_patches/NxThemesInstaller/theme.ips": b"theme",
        "atmosphere/package3": b"package3" * 100,
        "atmosphere/hosts/emummc.txt": b"127.0.0.1 *nintendo.com\n",
        "JKSV/SUPER MARIO ODYSSEY/Nub1234.zip": b"save backup" * 1000,
        "Nintendo/save/8000000000000124": b"sysmmc save",
        "emuMMC/RAW1/Nintendo/Contents/registered/00000097/game.nca/00": b"game",
        "bootloader/hekate_ipl.ini": b"[config]\n",
        "config/sys-patch/config.ini": b"[options]\npatch_sysmmc=0\n",
        "switch/DBI/DBI.nro": b"dbi",
        "hbmenu.nro": b"hbmenu",
    }
    for relative, data in protected.items():
        write(root, relative, data)
    if real:
        for name, folder in REAL.items():
            shutil.copytree(
                os.path.join(REAL_MODS, folder, "atmosphere"),
                os.path.join(root, LIB, name, "atmosphere"),
            )
    return root


def add_synthetic(root, name, seed):
    write(root, f"{LIB}/{name}/atmosphere/contents/0100000000010000/exefs/subsdk1", seed * 10)
    write(root, f"{LIB}/{name}/atmosphere/contents/0100000000010000/romfs/a/b.bin", seed * 3)
    write(root, f"{LIB}/{name}/atmosphere/exefs_patches/StarlightBase/3CA1.ips", seed)


def fresh_small():
    root = fresh(real=False)
    add_synthetic(root, "A", b"aaaa")
    add_synthetic(root, "B", b"bbbb")
    return root


def run(root, *args, crash=None, fail=None):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    if crash is not None:
        env["MS_CRASH_AT"] = str(crash)
    if fail is not None:
        env["MS_FAIL_AT"] = str(fail)
    try:
        done = subprocess.run(
            [CLI, root, *args], env=env, capture_output=True, text=True, errors="replace", timeout=60
        )
    except subprocess.TimeoutExpired:
        failures.append(f"hang args={args} crash={crash} fail={fail}")
        print("FAIL: hang", args, crash, fail)
        return 124, {"OPS": ["0"], "RESULT": ["-1", "hang"], "RECOVERED": [""], "ACTIVE": ["HANG", ""]}, ""
    fields = {}
    for line in done.stdout.splitlines():
        parts = line.split("\t")
        if parts[0] in ("OPS", "RESULT", "RECOVERED", "ACTIVE", "TRUNCATED"):
            fields[parts[0]] = parts[1:]
    if done.returncode not in (0, 1, 2, 99):
        failures.append(f"sanitizer or crash rc={done.returncode} args={args}: {done.stderr[:600]}")
        print("FAIL: bad exit", done.returncode, args, done.stderr[:600])
    return done.returncode, fields, done.stdout


def detect(root):
    _, fields, _ = run(root, "detect")
    return tuple(fields["ACTIVE"])


def listing(root):
    _, _, out = run(root, "list")
    result = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if parts[0] == "MOD":
            result[parts[2]] = parts[1] == "1"
    return result


def snapshot(base, skip_prefixes=(), ignore_names=()):
    result = {}
    for folder, subdirs, files in os.walk(base):
        relative_dir = os.path.relpath(folder, base)
        subdirs[:] = [
            d
            for d in sorted(subdirs)
            if not any(os.path.normpath(os.path.join(relative_dir, d)).startswith(p) for p in skip_prefixes)
            and d not in ignore_names
        ]
        for name in subdirs:
            result[os.path.normpath(os.path.join(relative_dir, name)) + "/"] = "DIR"
        for name in sorted(files):
            if name in ignore_names:
                continue
            relative = os.path.normpath(os.path.join(relative_dir, name))
            if any(relative.startswith(p) for p in skip_prefixes):
                continue
            with open(os.path.join(folder, name), "rb") as handle:
                result[relative] = hashlib.sha256(handle.read()).hexdigest()
    return result


def protected_snapshot(root):
    return snapshot(
        root,
        skip_prefixes=(ACTIVE_T, ACTIVE_P, "mods", WORKDIR, "config/odyssey-mod-switch"),
    )


def library_snapshot(root, names=None):
    result = {}
    base = os.path.join(root, LIB)
    for name in names if names is not None else sorted(os.listdir(base)):
        if name.startswith("_Recovered"):
            continue
        for key, value in snapshot(os.path.join(base, name)).items():
            result[f"{name}/{key}"] = value
    return result


def active_matches(root, name):
    lib_t = os.path.join(root, LIB, name, ACTIVE_T)
    lib_p = os.path.join(root, LIB, name, ACTIVE_P)
    active_t = os.path.join(root, ACTIVE_T)
    active_p = os.path.join(root, ACTIVE_P)
    ignore = IGNORED
    same_t = snapshot(lib_t, ignore_names=ignore) == snapshot(active_t, ignore_names=ignore)
    if os.path.isdir(lib_p):
        return same_t and snapshot(lib_p) == snapshot(active_p)
    return same_t and not os.path.exists(active_p)


def clean_work(root):
    work = os.path.join(root, WORKDIR)
    if not os.path.isdir(work):
        return True
    return not (os.path.exists(os.path.join(work, "stage")) or os.path.exists(os.path.join(work, "old")) or os.path.exists(os.path.join(work, "keep-cheats")))


# ---------------------------------------------------------------- basic behavior
def test_listing():
    root = fresh()
    mods = listing(root)
    check(list(mods) == ["SMO Online", "Shadow Clones", "Time Travel"], "lists three mods in sorted order")
    check(all(mods.values()), "all three real mods are usable")
    check(detect(root)[0] == "NONE", "starts with nothing active")


def test_all_swaps():
    names = list(REAL)
    for first in names:
        for second in names:
            root = fresh()
            before = protected_snapshot(root)
            library = library_snapshot(root)
            rc, _, _ = run(root, "enable", first)
            check(rc == 0, f"enable {first}")
            check(detect(root) == ("MOD", first), f"detects {first}")
            check(active_matches(root, first), f"active files match {first}")
            write(root, f"{ACTIVE_T}/romfs_metadata.bin", b"cache from atmosphere")
            check(detect(root) == ("MOD", first), f"cache file doesn't confuse detection of {first}")
            rc, _, _ = run(root, "enable", second)
            check(rc == 0, f"swap {first} to {second}")
            check(detect(root) == ("MOD", second), f"detects {second} after swap")
            check(active_matches(root, second), f"files match {second} after swap")
            if first != second:
                check(
                    not os.path.exists(os.path.join(root, ACTIVE_T, "romfs_metadata.bin")),
                    f"stale cache removed swapping {first} to {second}",
                )
            rc, _, _ = run(root, "disable")
            check(rc == 0, "disable after swap")
            check(detect(root)[0] == "NONE", "nothing active after disable")
            check(not os.path.exists(os.path.join(root, ACTIVE_T)), "game folder gone after disable")
            check(not os.path.exists(os.path.join(root, ACTIVE_P)), "patch folder gone after disable")
            check(protected_snapshot(root) == before, f"protected files untouched for {first}->{second}")
            check(library_snapshot(root) == library, f"library untouched for {first}->{second}")
            check(clean_work(root), "work folder left clean")


def test_idempotent():
    root = fresh_small()
    check(run(root, "disable")[0] == 0, "disable with nothing active succeeds")
    check(run(root, "disable")[0] == 0, "disable twice succeeds")
    check(run(root, "enable", "A")[0] == 0, "enable A")
    write(root, f"{ACTIVE_T}/romfs_metadata.bin", b"keep")
    check(run(root, "enable", "A")[0] == 0, "enable A again")
    check(
        os.path.exists(os.path.join(root, ACTIVE_T, "romfs_metadata.bin")),
        "enabling the active mod changes nothing",
    )
    check(run(root, "cleanup")[0] == 0, "cleanup is harmless")
    check(detect(root) == ("MOD", "A"), "cleanup keeps the active mod")


def test_junk_files():
    root = fresh_small()
    write(root, f"{LIB}/A/.DS_Store", b"junk")
    write(root, f"{LIB}/A/{ACTIVE_T}/._subsdk1", b"junk")
    write(root, f"{LIB}/A/{ACTIVE_T}/romfs/.DS_Store", b"junk")
    write(root, f"{LIB}/A/{ACTIVE_T}/{'romfs_metadata.bin'}", b"library cache")
    write(root, f"{LIB}/._hidden", b"junk")
    os.makedirs(os.path.join(root, LIB, ".hidden-folder"))
    check(list(listing(root)) == ["A", "B"], "hidden files aren't listed as mods")
    check(run(root, "enable", "A")[0] == 0, "enable a mod containing Mac junk")
    active = os.path.join(root, ACTIVE_T)
    check(not os.path.exists(os.path.join(active, "._subsdk1")), "._ files aren't copied")
    check(not os.path.exists(os.path.join(active, "romfs/.DS_Store")), ".DS_Store isn't copied")
    check(not os.path.exists(os.path.join(active, "romfs_metadata.bin")), "library's cache file isn't copied")
    write(root, f"{ACTIVE_T}/._new", b"junk")
    write(root, f"{ACTIVE_P}/.DS_Store", b"junk")
    check(detect(root) == ("MOD", "A"), "junk in active folders doesn't confuse detection")


def test_cheats():
    root = fresh_small()
    write(root, f"{ACTIVE_T}/cheats/3CA12DF.txt", b"my cheats")
    check(detect(root)[0] == "NONE", "cheats alone don't count as a mod")
    check(run(root, "enable", "A")[0] == 0, "enable with cheats present")
    cheat = os.path.join(root, ACTIVE_T, "cheats/3CA12DF.txt")
    check(open(cheat, "rb").read() == b"my cheats", "cheats survive enable")
    check(detect(root) == ("MOD", "A"), "cheats don't confuse detection")
    check(run(root, "enable", "B")[0] == 0, "swap with cheats present")
    check(open(cheat, "rb").read() == b"my cheats", "cheats survive swap")
    check(run(root, "disable")[0] == 0, "disable with cheats present")
    check(open(cheat, "rb").read() == b"my cheats", "cheats survive disable")
    check(detect(root)[0] == "NONE", "nothing active with only cheats left")
    write(root, f"{LIB}/A/{ACTIVE_T}/cheats/mod_cheat.txt", b"from mod")
    check(run(root, "enable", "A")[0] == 0, "enable a mod that ships cheats")
    check(not os.path.exists(os.path.join(root, ACTIVE_T, "cheats/mod_cheat.txt")), "a mod can't overwrite your cheats")
    check(open(cheat, "rb").read() == b"my cheats", "your cheats stay")


def unknown_cases():
    def edited(root):
        run(root, "enable", "A")
        write(root, f"{ACTIVE_T}/exefs/subsdk1", b"hand edited")

    def extra(root):
        run(root, "enable", "A")
        write(root, f"{ACTIVE_T}/romfs/extra.bin", b"extra")

    def missing(root):
        run(root, "enable", "A")
        os.remove(os.path.join(root, ACTIVE_T, "romfs/a/b.bin"))

    def only_game(root):
        write(root, f"{ACTIVE_T}/exefs/subsdk1", b"manual only game")

    def only_patch(root):
        write(root, f"{ACTIVE_P}/x.ips", b"manual only patch")

    def as_file(root):
        os.makedirs(os.path.join(root, "atmosphere/contents"), exist_ok=True)
        write(root, ACTIVE_T, b"a file where a folder belongs")

    return {
        "hand edited": edited,
        "extra file": extra,
        "missing file": missing,
        "only game": only_game,
        "only patch": only_patch,
        "file instead of folder": as_file,
    }


def test_unknown_never_lost():
    for label, setup in unknown_cases().items():
        for action in (("enable", "B"), ("disable",)):
            root = fresh_small()
            setup(root)
            check(detect(root)[0] == "UNKNOWN", f"{label}: detected as unknown")
            before = {}
            for part in (ACTIVE_T, ACTIVE_P):
                path = os.path.join(root, part)
                if os.path.isfile(path):
                    before[part] = hashlib.sha256(open(path, "rb").read()).hexdigest()
                elif os.path.isdir(path):
                    before[part] = snapshot(path)
            rc, fields, _ = run(root, *action)
            check(rc == 0, f"{label}: {action[0]} succeeds")
            recovered = fields["RECOVERED"][0] if fields["RECOVERED"] else ""
            check(recovered == "_Recovered-1", f"{label}: reports recovered folder")
            for part, expected in before.items():
                saved = os.path.join(root, LIB, recovered, part)
                if isinstance(expected, str):
                    check(hashlib.sha256(open(saved, "rb").read()).hexdigest() == expected, f"{label}: file kept")
                else:
                    check(snapshot(saved) == expected, f"{label}: {part} kept byte for byte")
            mods = listing(root)
            check(recovered in mods, f"{label}: recovered folder is listed")
            expected_now = ("MOD", "B") if action[0] == "enable" else ("NONE", "")
            check(detect(root)[0] == expected_now[0], f"{label}: state after {action[0]}")


def test_recovered_reusable():
    root = fresh_small()
    write(root, f"{ACTIVE_T}/exefs/subsdk1", b"manual mod")
    write(root, f"{ACTIVE_P}/x.ips", b"manual patch")
    run(root, "disable")
    check(listing(root).get("_Recovered-1") is True, "a recovered mod can be turned back on")
    check(run(root, "enable", "_Recovered-1")[0] == 0, "enable recovered mod")
    check(detect(root) == ("MOD", "_Recovered-1"), "recovered mod is detected")
    write(root, f"{ACTIVE_T}/exefs/subsdk1", b"edited again")
    rc, fields, _ = run(root, "disable")
    check(fields["RECOVERED"][0] == "_Recovered-2", "numbers don't collide")


def test_bad_names():
    root = fresh_small()
    run(root, "enable", "A")
    before = snapshot(root)
    for name in ["", ".", "..", "../..", "../../atmosphere", "A/../B", "a/b", "a\\b", "C:", ".hidden", "x" * 200]:
        rc, fields, _ = run(root, "enable", name)
        check(rc == 1, f"rejects name {name[:20]!r}")
        check(int(fields["RESULT"][0]) == 1, f"bad-name error for {name[:20]!r}")
    rc, fields, _ = run(root, "enable", "Does Not Exist")
    check(rc == 1 and int(fields["RESULT"][0]) == 2, "missing mod rejected")
    write(root, f"{LIB}/NotAFolder", b"file")
    rc, fields, _ = run(root, "enable", "NotAFolder")
    check(rc == 1 and int(fields["RESULT"][0]) == 2, "file as mod rejected")
    os.remove(os.path.join(root, LIB, "NotAFolder"))
    check(snapshot(root) == before, "rejected names changed nothing")


def test_bad_layouts():
    cases = {
        "other game": f"atmosphere/contents/0100000000020000/romfs/x.bin",
        "sysmodule": f"atmosphere/contents/420000000000000B/exefs.nsp",
        "other patch": f"atmosphere/exefs_patches/Evil/x.ips",
        "bootloader": f"bootloader/hekate_ipl.ini",
        "extra atmosphere folder": f"atmosphere/kips/loader.kip",
        "lunakit style": f"LunaKit/settings.ini",
    }
    for label, relative in cases.items():
        root = fresh_small()
        add_synthetic(root, "Bad", b"zzzz")
        write(root, f"{LIB}/Bad/{relative}", b"payload")
        run(root, "enable", "A")
        before = snapshot(root)
        check(listing(root)["Bad"] is False, f"{label}: marked unsupported")
        rc, fields, _ = run(root, "enable", "Bad")
        check(rc == 1 and int(fields["RESULT"][0]) == 3, f"{label}: rejected")
        check(snapshot(root) == before, f"{label}: nothing changed")
    for label, build in {
        "empty folder": lambda r: os.makedirs(os.path.join(r, LIB, "Bad")),
        "empty atmosphere": lambda r: os.makedirs(os.path.join(r, LIB, "Bad/atmosphere")),
        "atmosphere is a file": lambda r: write(r, f"{LIB}/Bad/atmosphere", b"x"),
        "game folder is a file": lambda r: write(r, f"{LIB}/Bad/{ACTIVE_T}", b"x"),
        "only junk": lambda r: write(r, f"{LIB}/Bad/{ACTIVE_T}/.DS_Store", b"x"),
    }.items():
        root = fresh_small()
        build(root)
        check(run(root, "enable", "Bad")[0] == 1, f"{label}: rejected")
        check(detect(root)[0] == "NONE", f"{label}: nothing activated")


def test_library_edges():
    root = fresh(real=False)
    check(listing(root) == {}, "no mods folder: empty list")
    check(run(root, "disable")[0] == 0, "no mods folder: disable OK")
    check(run(root, "enable", "A")[0] == 1, "no mods folder: enable fails cleanly")
    os.makedirs(os.path.join(root, LIB))
    check(listing(root) == {}, "empty mods folder: empty list")

    root = fresh(real=False)
    write(root, f"{ACTIVE_T}/exefs/subsdk1", b"manual, no library")
    check(detect(root)[0] == "UNKNOWN", "active files with no library: unknown")
    check(run(root, "disable")[0] == 0, "active files with no library: disable OK")
    check(listing(root).get("_Recovered-1") is True, "active files with no library: kept")

    root = fresh(real=False)
    for i in range(70):
        add_synthetic(root, f"Mod {i:03d}", bytes([i + 1]) * 4)
    _, fields, out = run(root, "list")
    check(out.count("MOD\t") == 64, "list caps at 64 mods")
    check(fields["TRUNCATED"][0] == "1", "cap is reported")
    check(run(root, "enable", "Mod 069")[0] == 0, "a mod past the cap can still be enabled by name")

    root = fresh(real=False)
    name = "Super Long Mod Name With Spaces and (Parens) [v1.0] -- ünïcödé ✓"
    add_synthetic(root, name, b"uuuu")
    check(run(root, "enable", name)[0] == 0, "unicode and punctuation in a name")
    check(detect(root) == ("MOD", name), "unicode name detected")

    root = fresh(real=False)
    limit = "n" * 127
    add_synthetic(root, limit, b"llll")
    check(run(root, "enable", limit)[0] == 0, "127 character name works")
    too_long = "n" * 128
    add_synthetic(root, too_long, b"LLLL")
    check(too_long not in listing(root), "128 character name isn't listed")
    check(run(root, "enable", too_long)[0] == 1, "128 character name rejected")

    root = fresh_small()
    add_synthetic(root, "A Copy", b"aaaa")
    check(run(root, "enable", "A Copy")[0] == 0, "two identical mods: enable the copy")
    check(detect(root)[0] == "MOD", "two identical mods: detected as one of them")
    check(active_matches(root, "A Copy"), "two identical mods: files correct")
    check(run(root, "enable", "A")[0] == 0, "two identical mods: enable the original")
    check(run(root, "enable", "B")[0] == 0, "two identical mods: can swap away")
    check(detect(root) == ("MOD", "B"), "two identical mods: swap landed")


def test_file_sizes():
    root = fresh(real=False)
    add_synthetic(root, "Big", b"bbbb")
    sizes = {
        "empty.bin": 0,
        "one.bin": 1,
        "exact.bin": 64 * 1024,
        "exact_minus.bin": 64 * 1024 - 1,
        "exact_plus.bin": 64 * 1024 + 1,
        "double.bin": 128 * 1024,
        "large.bin": 12 * 1024 * 1024 + 7,
    }
    for name, size in sizes.items():
        write(root, f"{LIB}/Big/{ACTIVE_T}/romfs/{name}", os.urandom(size))
    os.makedirs(os.path.join(root, LIB, "Big", ACTIVE_T, "romfs/empty_dir"))
    check(run(root, "enable", "Big")[0] == 0, "enable files of tricky sizes")
    check(active_matches(root, "Big"), "tricky sizes copied exactly, including empty folders")
    active = os.path.join(root, ACTIVE_T, "romfs/large.bin")
    with open(active, "r+b") as handle:
        handle.seek(9 * 1024 * 1024)
        byte = handle.read(1)
        handle.seek(9 * 1024 * 1024)
        handle.write(bytes([byte[0] ^ 1]))
    check(detect(root)[0] == "UNKNOWN", "a single flipped bit is noticed")


def test_many_files():
    root = fresh(real=False)
    add_synthetic(root, "Many", b"mmmm")
    add_synthetic(root, "Other", b"oooo")
    for i in range(400):
        write(root, f"{LIB}/Many/{ACTIVE_T}/romfs/flat/file{i:04d}.bin", bytes([i % 251]) * 10)
    for i in range(40):
        write(root, f"{LIB}/Many/{ACTIVE_T}/romfs/nested/{i}/sub/f.bin", bytes([i]) * 5)
    check(run(root, "enable", "Many")[0] == 0, "enable a mod with hundreds of files")
    check(active_matches(root, "Many"), "hundreds of files copied exactly")
    check(run(root, "enable", "Other")[0] == 0, "swap away from hundreds of files")
    check(active_matches(root, "Other"), "no files left behind after swap")
    check(clean_work(root), "work folder empty after removing hundreds of files")
    check(run(root, "disable")[0] == 0, "disable after many files")
    check(detect(root)[0] == "NONE", "nothing active after many files")


def test_deep_and_long_paths():
    root = fresh_small()
    run(root, "enable", "A")
    before = snapshot(root)
    deep = "/".join(["d"] * 30)
    write(root, f"{LIB}/Deep/{ACTIVE_T}/romfs/{deep}/x.bin", b"deep")
    rc, fields, _ = run(root, "enable", "Deep")
    check(rc == 1, "too-deep mod rejected")
    check(detect(root) == ("MOD", "A"), "too-deep mod leaves the active mod running")
    check(active_matches(root, "A"), "too-deep mod leaves active files intact")
    shutil.rmtree(os.path.join(root, LIB, "Deep"))
    check({k: v for k, v in snapshot(root).items() if WORKDIR not in k} == {k: v for k, v in before.items() if WORKDIR not in k}, "too-deep mod changed nothing else")


def test_symlinks():
    root = fresh_small()
    target = os.path.join(root, "hbmenu.nro")
    os.symlink(target, os.path.join(root, LIB, "A", ACTIVE_T, "romfs/link.bin"))
    run(root, "enable", "B")
    check(run(root, "enable", "A")[0] == 1, "symlink in a mod is rejected")
    check(detect(root) == ("MOD", "B"), "symlink rejection leaves active mod")
    check(open(target, "rb").read() == b"hbmenu", "symlink target untouched")


def test_permission_failures():
    # A blocked work folder acts like a card that refuses writes.
    root = fresh_small()
    run(root, "enable", "A")
    shutil.rmtree(os.path.join(root, "config/odyssey-mod-switch"))
    write(root, "config/odyssey-mod-switch", b"a file blocks the work folder")
    rc, _, _ = run(root, "enable", "B")
    check(rc == 1, "blocked work folder fails cleanly")
    check(detect(root) == ("MOD", "A"), "blocked work folder leaves active mod")
    check(active_matches(root, "A"), "blocked work folder leaves files intact")
    os.remove(os.path.join(root, "config/odyssey-mod-switch"))
    check(run(root, "enable", "B")[0] == 0, "recovers once the block is gone")

    root = fresh_small()
    run(root, "enable", "A")
    os.chmod(os.path.join(root, LIB, "B", ACTIVE_T, "romfs/a/b.bin"), 0o000)
    rc, _, _ = run(root, "enable", "B")
    check(rc == 1, "unreadable mod file fails cleanly")
    check(detect(root) == ("MOD", "A"), "unreadable mod file leaves active mod")
    os.chmod(os.path.join(root, LIB, "B", ACTIVE_T, "romfs/a/b.bin"), 0o644)
    check(run(root, "enable", "B")[0] == 0, "recovers once the file is readable")


def test_missing_parent_folders():
    root = fresh_small()
    shutil.rmtree(os.path.join(root, "atmosphere/exefs_patches"))
    shutil.rmtree(os.path.join(root, "config"))
    check(run(root, "enable", "A")[0] == 0, "creates missing parent folders")
    check(active_matches(root, "A"), "files correct after creating parents")


def test_leftovers():
    root = fresh_small()
    run(root, "enable", "A")
    write(root, f"{WORKDIR}/stage/contents/junk.bin", b"old stage")
    write(root, f"{WORKDIR}/old/contents/junk.bin", b"old active")
    check(run(root, "enable", "B")[0] == 0, "stale work folders are cleared")
    check(active_matches(root, "B"), "stale stage isn't installed")
    check(clean_work(root), "stale work folders removed")

    root = fresh_small()
    write(root, f"{WORKDIR}/keep-cheats/c.txt", b"parked")
    write(root, f"{ACTIVE_T}/cheats/d.txt", b"current")
    check(run(root, "cleanup")[0] == 0, "cleanup with two cheats folders")
    check(os.path.exists(os.path.join(root, ACTIVE_T, "cheats/d.txt")), "current cheats stay")
    check(os.path.exists(os.path.join(root, WORKDIR, "cheats-conflict-1/c.txt")), "parked cheats kept, not deleted")


# ---------------------------------------------------------- power cuts and errors
SCENARIOS = {}


def scenario(fn):
    SCENARIOS[fn.__name__] = fn
    return fn


@scenario
def enable_from_nothing(root):
    return ("enable", "A"), "A", None


@scenario
def swap_mods(root):
    run(root, "enable", "B")
    write(root, f"{ACTIVE_T}/romfs_metadata.bin", b"cache")
    return ("enable", "A"), "A", None


@scenario
def enable_keeping_cheats(root):
    run(root, "enable", "B")
    write(root, f"{ACTIVE_T}/cheats/c.txt", b"cheats")
    return ("enable", "A"), "A", None


@scenario
def enable_over_unknown(root):
    write(root, f"{ACTIVE_T}/exefs/subsdk1", b"manual")
    write(root, f"{ACTIVE_P}/x.ips", b"manual patch")
    return ("enable", "A"), "A", b"manual"


@scenario
def disable_active(root):
    run(root, "enable", "B")
    return ("disable",), None, None


@scenario
def disable_keeping_cheats(root):
    run(root, "enable", "B")
    write(root, f"{ACTIVE_T}/cheats/c.txt", b"cheats")
    return ("disable",), None, None


@scenario
def disable_unknown(root):
    write(root, f"{ACTIVE_T}/exefs/subsdk1", b"manual")
    write(root, f"{ACTIVE_P}/x.ips", b"manual patch")
    return ("disable",), None, b"manual"


def everything_has(root, needle):
    for folder, _, files in os.walk(root):
        for name in files:
            with open(os.path.join(folder, name), "rb") as handle:
                if handle.read() == needle:
                    return True
    return False


def count_ops(fn):
    root = fresh_small()
    args, _, _ = fn(root)
    _, fields, _ = run(root, *args)
    return int(fields["OPS"][0])


def test_power_cut_everywhere():
    total = 0
    for name, fn in SCENARIOS.items():
        ops = count_ops(fn)
        check(ops > 0, f"{name}: counted operations")
        for k in range(1, ops + 1):
            root = fresh_small()
            args, _, unknown = fn(root)
            had_cheats = os.path.exists(os.path.join(root, ACTIVE_T, "cheats/c.txt"))
            protected = protected_snapshot(root)
            library = library_snapshot(root, ["A", "B"])
            rc, _, _ = run(root, *args, crash=k)
            check(rc == 99, f"{name}: power cut fired at op {k}")
            verify_after_interruption_lib(root, f"{name} cut@{k}", protected, library, had_cheats, unknown)
            total += 1
    print(f"  power cut points tested: {total}")


def verify_after_interruption_lib(root, label, protected, library, had_cheats, unknown):
    # Interrupted runs may add _Recovered folders, so only the originals are compared.
    def lib():
        return library_snapshot(root, ["A", "B"])

    check(protected_snapshot(root) == protected, f"{label}: protected files untouched")
    check(lib() == library, f"{label}: library untouched")
    if unknown is not None:
        check(everything_has(root, unknown), f"{label}: unknown files not lost")
    state = detect(root)
    if state[0] == "MOD":
        check(active_matches(root, state[1]), f"{label}: detection only trusts exact matches")
    check(run(root, "cleanup")[0] == 0, f"{label}: cleanup succeeds")
    check(clean_work(root), f"{label}: work folder clean")
    if had_cheats:
        check(os.path.exists(os.path.join(root, ACTIVE_T, "cheats/c.txt")), f"{label}: cheats restored")
    if unknown is not None:
        check(everything_has(root, unknown), f"{label}: unknown kept after cleanup")
    check(run(root, "enable", "B")[0] == 0, f"{label}: can enable afterward")
    check(detect(root) == ("MOD", "B"), f"{label}: correct mod afterward")
    check(active_matches(root, "B"), f"{label}: exact files afterward")
    if had_cheats:
        check(os.path.exists(os.path.join(root, ACTIVE_T, "cheats/c.txt")), f"{label}: cheats kept after enable")
    if unknown is not None:
        check(everything_has(root, unknown), f"{label}: unknown kept after enable")
    check(run(root, "disable")[0] == 0, f"{label}: can disable afterward")
    check(detect(root)[0] == "NONE", f"{label}: nothing active afterward")
    check(protected_snapshot(root) == protected, f"{label}: protected still untouched")
    check(lib() == library, f"{label}: library still untouched")


def test_io_error_everywhere():
    total = 0
    for name, fn in SCENARIOS.items():
        ops = count_ops(fn)
        for k in range(1, ops + 1):
            root = fresh_small()
            args, _, unknown = fn(root)
            had_cheats = os.path.exists(os.path.join(root, ACTIVE_T, "cheats/c.txt"))
            protected = protected_snapshot(root)
            library = library_snapshot(root, ["A", "B"])
            rc, _, _ = run(root, *args, fail=k)
            check(rc in (0, 1), f"{name}: error at op {k} reported cleanly")
            verify_after_interruption_lib(root, f"{name} err@{k}", protected, library, had_cheats, unknown)
            total += 1
    print(f"  I/O error points tested: {total}")


def test_copy_failure_keeps_current_mod():
    # A failed copy, such as a full card, must leave the running mod in place.
    fn = SCENARIOS["swap_mods"]
    ops = count_ops(fn)
    root = fresh_small()
    fn(root)
    files = len([k for k in snapshot(os.path.join(root, LIB, "A")) if not k.endswith("/")])
    first_change = None
    for k in range(1, ops + 1):
        root = fresh_small()
        args, _, _ = fn(root)
        rc, _, _ = run(root, *args, fail=k)
        still_b = active_matches(root, "B")
        if rc != 0 and not still_b and first_change is None:
            first_change = k
    check(first_change is None or first_change > files, f"all file writes finish before active files change (first change at {first_change}, {files} files)")


def test_double_power_cut():
    # Power cut during the swap, then another during recovery.
    total = 0
    for name in ("swap_mods", "enable_keeping_cheats", "disable_keeping_cheats", "enable_over_unknown"):
        fn = SCENARIOS[name]
        ops = count_ops(fn)
        for k in range(1, ops + 1, 2):
            probe = fresh_small()
            args, _, _ = fn(probe)
            run(probe, *args, crash=k)
            _, fields, _ = run(probe, "cleanup")
            recovery_ops = int(fields["OPS"][0])
            for j in range(1, recovery_ops + 1):
                root = fresh_small()
                args, _, unknown = fn(root)
                had_cheats = os.path.exists(os.path.join(root, ACTIVE_T, "cheats/c.txt"))
                protected = protected_snapshot(root)
                library = library_snapshot(root, ["A", "B"])
                run(root, *args, crash=k)
                rc, _, _ = run(root, "cleanup", crash=j)
                check(rc == 99, f"{name}: second cut fired at {k}/{j}")
                verify_after_interruption_lib(root, f"{name} double {k}/{j}", protected, library, had_cheats, unknown)
                total += 1
    print(f"  double power cut combinations tested: {total}")


def main():
    tests = [
        test_listing,
        test_all_swaps,
        test_idempotent,
        test_junk_files,
        test_cheats,
        test_unknown_never_lost,
        test_recovered_reusable,
        test_bad_names,
        test_bad_layouts,
        test_library_edges,
        test_file_sizes,
        test_many_files,
        test_deep_and_long_paths,
        test_symlinks,
        test_permission_failures,
        test_missing_parent_folders,
        test_leftovers,
        test_power_cut_everywhere,
        test_io_error_everywhere,
        test_copy_failure_keeps_current_mod,
        test_double_power_cut,
    ]
    only = os.environ.get("TEST_FILTER")
    for test in tests:
        if only and only not in test.__name__:
            continue
        count = len(failures)
        before = passed
        test()
        status = "ok" if len(failures) == count else f"{len(failures) - count} FAILED"
        print(f"{test.__name__}: {passed - before} checks, {status}")
    print(f"\nTOTAL: {passed} passed, {len(failures)} failed")
    for failure in failures[:40]:
        print(" -", failure)
    sys.exit(1 if failures else 0)


main()
