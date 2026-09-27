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
        --release 1.0.1:win-x64:path/to/1.0.1.spk:path/to/1.0.1.spk.zst:<url-base> \\
        --delta 1.0.1:1.0.0:path/to/1.0.0-1.0.1.patch \\
        --release 1.0.0:win-x64:path/to/1.0.0.spk::<url-base>

Recent releases are listed as well as the new one, and not out of tidiness: a client
checks its own installation against the archive hash of the release it came from before
it patches from it (step 4 of CheckAndStage), and it can only do that for a version the
manifest still remembers.
"""

import argparse
import hashlib
import json
import pathlib
import sys


def digest(path: pathlib.Path) -> str:
    """BLAKE2b-256 of a file, streamed: a package is 213 MiB."""
    hasher = hashlib.blake2b(digest_size=32)
    with path.open("rb") as handle:
        while chunk := handle.read(1 << 20):
            hasher.update(chunk)
    return hasher.hexdigest()


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--generated-at-ms", type=int, default=0)
    parser.add_argument(
        "--release",
        action="append",
        default=[],
        metavar="VERSION:PLATFORM:SPK:SPKZST:URLBASE",
        help="a release: its version, its platform, its uncompressed archive, its "
        "compressed package, and where that package will be served from",
    )
    parser.add_argument(
        "--delta",
        action="append",
        default=[],
        metavar="TOVERSION:FROMVERSION:PATCH",
        help="a patch belonging to one of the releases above",
    )
    parser.add_argument("--url-base", default="", help="prepended to every artefact name")
    arguments = parser.parse_args()

    releases: dict[tuple[str, str], dict] = {}
    for spec in arguments.release:
        fields = spec.split(":")
        if len(fields) < 4:
            raise SystemExit(f"--release {spec!r}: expected VERSION:PLATFORM:SPK:SPKZST[:URLBASE]")
        version, platform, spk, spkzst = fields[0], fields[1], fields[2], fields[3]
        url_base = ":".join(fields[4:]) if len(fields) > 4 else arguments.url_base
        release: dict = {
            "version": version,
            "platform": platform,
            # No url: the uncompressed archive is never served. Its size and hash are
            # here because they are the precondition for everything that is.
            "archive": artifact(pathlib.Path(spk), None),
        }
        if not spkzst:
            raise SystemExit(f"--release {spec!r}: a release needs a package to fall back to")
        name = pathlib.Path(spkzst).name
        release["package"] = artifact(pathlib.Path(spkzst), f"{url_base}/{name}")
        releases[(version, platform)] = release

    for spec in arguments.delta:
        to_version, from_version, patch = spec.split(":", 2)
        matches = [key for key in releases if key[0] == to_version]
        if not matches:
            raise SystemExit(f"--delta {spec!r}: no release {to_version} to attach it to")
        for key in matches:
            release = releases[key]
            url_base = release.get("package", {}).get("url", "").rsplit("/", 1)[0]
            name = pathlib.Path(patch).name
            entry = artifact(pathlib.Path(patch), f"{url_base}/{name}")
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
