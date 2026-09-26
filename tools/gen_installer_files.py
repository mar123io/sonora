#!/usr/bin/env python3
"""Generate the WiX fragment that lists everything the MSI ships.

Why this exists: WiX v4 has no way to harvest a directory. The `Files` element
that does it in one line arrived in v5, and v5 is where the Open Source
Maintenance Fee licence gate starts -- so the choice was a licence or a
generator, and a generator is a hundred lines.

WiX v3 shipped `heat` for this, and v4 dropped it. That is not much of a loss:
heat's output was also generated, just by a tool nobody could read, and this
project already generates its bridge, its embedded assets and its icon from
scripts in this directory. One more script is a smaller cost than one more
dependency.

What it emits, for a payload of about 250 files (CEF alone is libcef.dll, the v8
snapshots, four .pak files, icudtl.dat and every locale):

  * a Directory tree under INSTALLFOLDER, one element per subdirectory
  * one Component per file, with the file as its key path

One file per component is the shape Windows Installer wants, and the one that
lets a repair or a patch touch a single file. Component GUIDs are deliberately
left out: WiX derives a stable one from the component's target path, so the same
file installed to the same place keeps its identity across releases -- which is
what an upgrade needs and what a freshly random GUID per build would destroy.

Usage:
    python tools/gen_installer_files.py --stage build/win-release/stage \\
                                        --out build/win-release/generated/payload.wxs
"""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

# A WiX identifier is letters, digits, underscores and dots, and must not start
# with a digit. Everything else in a file name -- the hyphen in zh-CN.pak, a
# space, anything non-ASCII -- has to go.
_INVALID = re.compile(r"[^A-Za-z0-9_.]")


def identifier(prefix: str, relative: str) -> str:
    """A unique, valid WiX Id for a path relative to the payload root."""
    cleaned = _INVALID.sub("_", relative.replace("\\", "/").replace("/", "_"))
    if not cleaned or not (cleaned[0].isalpha() or cleaned[0] == "_"):
        cleaned = "_" + cleaned

    name = f"{prefix}_{cleaned}"
    if len(name) <= 72:
        return name

    # Ids are limited to 72 characters. Nothing in this payload comes close, but
    # a payload is a moving target and a truncation that silently collided would
    # be a very unpleasant afternoon. The digest is of the full path, so two
    # names that truncate to the same thing still differ.
    digest = hashlib.sha256(relative.encode("utf-8")).hexdigest()[:8]
    keep = 72 - len(prefix) - len(digest) - 2
    return f"{prefix}_{cleaned[:keep]}_{digest}"


def escape(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def build_tree(directories: list[str]) -> dict:
    """Nested dict of directory name -> children, from a list of relative paths."""
    tree: dict = {}
    for relative in directories:
        node = tree
        for part in relative.split("/"):
            node = node.setdefault(part, {})
    return tree


def emit_directories(tree: dict, prefix: str, ids: dict[str, str], depth: int) -> list[str]:
    """Directory elements, properly nested, in sorted order."""
    lines: list[str] = []
    indent = "      " + "  " * depth
    for name in sorted(tree):
        relative = f"{prefix}/{name}" if prefix else name
        ids[relative] = identifier("dir", relative)
        children = emit_directories(tree[name], relative, ids, depth + 1)
        if children:
            lines.append(f'{indent}<Directory Id="{ids[relative]}" Name="{escape(name)}">')
            lines.extend(children)
            lines.append(f"{indent}</Directory>")
        else:
            lines.append(f'{indent}<Directory Id="{ids[relative]}" Name="{escape(name)}" />')
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()

    stage: Path = arguments.stage.resolve()
    if not stage.is_dir():
        raise SystemExit(f"gen_installer_files: {stage} is not a directory")

    # Sorted, so the same payload produces the same fragment byte for byte. A
    # generator whose output depends on whatever order the filesystem felt like
    # is a generator that makes every build look like a change.
    files = sorted(p for p in stage.rglob("*") if p.is_file())
    if not files:
        raise SystemExit(f"gen_installer_files: nothing in {stage}")

    directories = sorted(
        {p.parent.relative_to(stage).as_posix() for p in files} - {"."}
    )

    ids: dict[str, str] = {"": "INSTALLFOLDER"}
    directory_lines = emit_directories(build_tree(directories), "", ids, 0)

    lines: list[str] = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generated by tools/gen_installer_files.py. Do not edit. -->",
        '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
        "  <Fragment>",
        '    <DirectoryRef Id="INSTALLFOLDER">',
        *directory_lines,
        "    </DirectoryRef>",
        "",
        '    <ComponentGroup Id="Payload">',
    ]

    for path in files:
        relative = path.relative_to(stage).as_posix()
        parent = path.parent.relative_to(stage).as_posix()
        directory_id = ids["" if parent == "." else parent]
        lines.append(
            f'      <Component Directory="{directory_id}" Id="{identifier("cmp", relative)}">'
        )
        lines.append(
            f'        <File Id="{identifier("fil", relative)}" '
            f'Source="{escape(str(path))}" KeyPath="yes" />'
        )
        lines.append("      </Component>")

    lines.append("    </ComponentGroup>")
    lines.append("  </Fragment>")
    lines.append("</Wix>")

    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(
        f"gen_installer_files: {len(files)} file(s), "
        f"{len(directories)} directory(ies) -> {arguments.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
