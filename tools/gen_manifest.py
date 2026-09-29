#!/usr/bin/env python3
"""Writes the update manifest.

The fifth generator in this repository, after the bridge, the assets, the icon and the
installer's file list -- and, like all of them, it exists because the alternative was a
list maintained by a person.

It is Python and not C++, unlike sonora_release next to it, and that split is on
purpose. sonora_release produces *bytes* that a client will later reproduce or verify,
so it has to be the same code as the client: the archive format, the hash and the patch
parameters live in one place and nowhere else. This script produces a *document* whose
only consumer is a parser written in C++ with 40 test cases pointed at it, so a second
implementation here is a second pair of eyes rather than a second source of truth.

One thing it computes that the client also computes: the hash. BLAKE2b-256 is
hashlib.blake2b(digest_size=32), which is exactly libsodium's crypto_generichash with a
32-byte output and no key -- and tests/test_update_archive.cpp checks three known
answers against this, because "they agree because both call libsodium" is not true here.

Usage:

    gen_manifest.py --out manifest.json --channel stable \\
        --release "1.0.1|win-x64|path/to/1.0.1.spk|path/to/1.0.1.spk.zst|<url-base>" \\
        --delta "1.0.1|1.0.0|path/to/1.0.0-1.0.1.patch" \\
        --release "1.0.0|win-x64|path/to/1.0.0.spk|path/to/1.0.0.spk.zst|<url-base>"

Recent releases are listed as well as the new one, and not out of tidiness: a client
checks its own installation against the archive hash of the release it came from before
it patches from it (step 4 of CheckAndStage), and it can only do that for a version the
manifest still remembers.

The fields are separated by a vertical bar, and that is the second thing this file gets
asked about. It was a colon, which reads better and cannot work: three of the five fields
are a path or a URL, and on Windows a path begins "D:\\" while every URL contains "://".
The parser split on every colon and took the drive letter as the filename, so a release
built from an absolute path died with

    FileNotFoundError: [Errno 2] No such file or directory: \'D\'

and it died in the one code path nothing had ever run: until v1.1.0 there was no earlier
release carrying a package, so no previous release was ever listed and no delta was ever
attached. A bar cannot appear in a Windows path -- the filesystem refuses it -- and does
not appear unencoded in a URL, which is the whole reason to prefer an ugly separator to a
pretty one. `--self-test` below is what now runs that path on every push.
"""

import argparse
import hashlib
import json
import pathlib
import sys
import tempfile

# Not a colon. See the note in the module docstring: three of the five fields in a
# --release spec can contain one, and two of them always do.
SEPARATOR = "|"


def digest(path: pathlib.Path) -> str:
    """BLAKE2b-256 of a file, streamed: a package is 213 MiB."""
    hasher = hashlib.blake2b(digest_size=32)
    with path.open("rb") as handle:
        while chunk := handle.read(1 << 20):
            hasher.update(chunk)
    return hasher.hexdigest()


def named_file(text: str, what: str, spec: str) -> pathlib.Path:
    """The path a spec names, or a refusal that says which field was wrong.

    Before, a field that was not a path at all reached pathlib and failed inside stat()
    with the name of something nobody had written down -- which is how a drive letter
    ended up looking like a missing file.
    """
    if not text:
        raise SystemExit(f"--release {spec!r}: the {what} field is empty")
    path = pathlib.Path(text)
    if not path.is_file():
        raise SystemExit(
            f"--release {spec!r}: the {what} field names {text!r}, which is not a file.\n"
            f"    Fields are separated by {SEPARATOR!r}; a path that contains one cannot be "
            f"passed through this interface."
        )
    return path


def artifact(path: pathlib.Path, url: str | None) -> dict:
    entry: dict = {}
    if url:
        # The order matters only to a person reading the file, which is reason enough:
        # url, then size, then hash, so that the eye finds the name first.
        entry["url"] = url
    entry["size"] = path.stat().st_size
    entry["hash"] = digest(path)
    if entry["size"] == 0:
        raise SystemExit(f"{path} is empty; a release cannot name it")
    return entry


def self_test() -> int:
    """Runs the release path end to end, in a temporary directory, on every push.

    This exists because of what it would have caught. The two-release, one-delta shape is
    what a release produces the moment a previous release published a package -- and no
    release before v1.1.0 ever did, so between week 11 and here the delta half of this
    script ran nowhere at all. It was wrong in two ways at once, and the second would have
    surfaced only after the first was fixed. Fifteen minutes of a Windows runner per
    attempt, on a tag.

    The absolute path below is the point of the whole test. It is the shape the packaging
    job hands over on a real runner, and it is what turned a drive letter into a filename.
    """
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        files = {
            "new_spk": root / "Sonora-1.1.0-win-x64.spk",
            "new_zst": root / "Sonora-1.1.0-win-x64.spk.zst",
            "old_spk": root / "prev" / "1.0.0.spk",
            "old_zst": root / "prev" / "Sonora-1.0.0-win-x64.spk.zst",
            "patch": root / "1.0.0-1.1.0-win-x64.patch",
        }
        for path in files.values():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(path.name.encode("utf-8") * 64)

        out = root / "manifest.json"
        base = "https://github.com/owner/repo/releases/download"
        # Windows-shaped and absolute on purpose, even here: the string is what is being
        # parsed, and a separator that survives it on Linux survives it there.
        windows_style = "D:\\a\\sonora\\sonora\\" + files["old_zst"].name
        code = main(
            [
                "--out", str(out),
                "--channel", "stable",
                "--release",
                SEPARATOR.join(["1.1.0", "win-x64", str(files["new_spk"]),
                                str(files["new_zst"]), f"{base}/v1.1.0"]),
                "--release",
                SEPARATOR.join(["1.0.0", "win-x64", str(files["old_spk"]),
                                str(files["old_zst"]), f"{base}/v1.0.0"]),
                "--delta",
                SEPARATOR.join(["1.1.0", "1.0.0", str(files["patch"])]),
            ]
        )
        if code != 0:
            raise SystemExit(f"self-test: the generator returned {code}")

        document = json.loads(out.read_text(encoding="utf-8"))
        versions = [release["version"] for release in document["releases"]]
        if versions != ["1.1.0", "1.0.0"]:
            raise SystemExit(f"self-test: expected 1.1.0 then 1.0.0, got {versions}")

        newest = document["releases"][0]
        if len(newest.get("deltas", [])) != 1:
            raise SystemExit("self-test: the newest release should carry exactly one delta")
        delta = newest["deltas"][0]
        if delta["from"] != "1.0.0":
            raise SystemExit(f"self-test: the delta is from {delta['from']!r}, not 1.0.0")
        if not delta["url"].endswith(files["patch"].name):
            raise SystemExit(f"self-test: the delta's url is {delta['url']!r}")
        for release in document["releases"]:
            for key in ("archive", "package"):
                if release[key]["size"] <= 0 or len(release[key]["hash"]) != 64:
                    raise SystemExit(f"self-test: {release['version']} has no usable {key}")

        # And the failure that started this: a field that is not a file is refused by name
        # rather than dying inside stat() with a drive letter for a filename.
        try:
            main(
                [
                    "--out", str(out),
                    "--release",
                    SEPARATOR.join(["1.0.0", "win-x64", str(files["old_spk"]),
                                    windows_style, f"{base}/v1.0.0"]),
                ]
            )
        except SystemExit as refusal:
            if "not a file" not in str(refusal):
                raise SystemExit(f"self-test: refused, but unhelpfully: {refusal}") from None
        else:
            raise SystemExit("self-test: a package that does not exist was accepted")

    print("gen_manifest: self-test passed -- 2 releases, 1 delta, 1 refusal", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    # Not required, because --self-test writes into a directory it makes itself.
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--generated-at-ms", type=int, default=0)
    parser.add_argument(
        "--release",
        action="append",
        default=[],
        metavar="VERSION|PLATFORM|SPK|SPKZST|URLBASE",
        help="a release: its version, its platform, its uncompressed archive, its "
        "compressed package, and where that package will be served from",
    )
    parser.add_argument(
        "--delta",
        action="append",
        default=[],
        metavar="TOVERSION|FROMVERSION|PATCH",
        help="a patch belonging to one of the releases above",
    )
    parser.add_argument("--url-base", default="", help="prepended to every artefact name")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="build a two-release manifest with a delta in a temporary directory and "
        "check it, then exit. This is the path that a release exercises and that nothing "
        "else does.",
    )
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        return self_test()
    if arguments.out is None:
        parser.error("--out is required")

    releases: dict[tuple[str, str], dict] = {}
    for spec in arguments.release:
        fields = spec.split(SEPARATOR)
        if len(fields) not in (4, 5):
            raise SystemExit(
                f"--release {spec!r}: expected "
                f"VERSION{SEPARATOR}PLATFORM{SEPARATOR}SPK{SEPARATOR}SPKZST"
                f"[{SEPARATOR}URLBASE], separated by {SEPARATOR!r}"
            )
        version, platform, spk, spkzst = fields[0], fields[1], fields[2], fields[3]
        url_base = fields[4] if len(fields) > 4 else arguments.url_base
        release: dict = {
            "version": version,
            "platform": platform,
            # No url: the uncompressed archive is never served. Its size and hash are
            # here because they are the precondition for everything that is.
            "archive": artifact(named_file(spk, "archive", spec), None),
        }
        package = named_file(spkzst, "package", spec)
        name = package.name
        release["package"] = artifact(package, f"{url_base}/{name}")
        releases[(version, platform)] = release

    for spec in arguments.delta:
        fields = spec.split(SEPARATOR)
        if len(fields) != 3:
            raise SystemExit(
                f"--delta {spec!r}: expected "
                f"TOVERSION{SEPARATOR}FROMVERSION{SEPARATOR}PATCH"
            )
        to_version, from_version, patch = fields
        matches = [key for key in releases if key[0] == to_version]
        if not matches:
            raise SystemExit(f"--delta {spec!r}: no release {to_version} to attach it to")
        for key in matches:
            release = releases[key]
            url_base = release.get("package", {}).get("url", "").rsplit("/", 1)[0]
            patch_file = named_file(patch, "patch", spec)
            name = patch_file.name
            entry = artifact(patch_file, f"{url_base}/{name}")
            entry_with_from = {"from": from_version, **entry}
            release.setdefault("deltas", []).append(entry_with_from)

    document = {
        "schema": 1,
        "channel": arguments.channel,
    }
    if arguments.generated_at_ms:
        document["generated_at_ms"] = arguments.generated_at_ms
    # Newest first, by version and not by the spelling of one: "1.10.0" sorts before
    # "1.9.0" as a string, and a document a person reads to check a release should not be
    # the place that mistake shows up.
    def order(key: tuple[str, str]) -> tuple:
        return tuple(int(part) for part in key[0].split(".")), key[1]

    document["releases"] = [releases[key] for key in sorted(releases, key=order, reverse=True)]

    if not document["releases"]:
        raise SystemExit("a manifest with no releases in it would say that nobody can update")

    # Two-space indent and a trailing newline are not cosmetic: this document is signed
    # byte for byte, so whatever is written here is what gets signed and what the client
    # verifies. Changing the formatting changes the signature, which is exactly right.
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"{arguments.out}: {len(document['releases'])} releases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
