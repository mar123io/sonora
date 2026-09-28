#!/usr/bin/env python3
"""Checks that every internal link in this repository points at a file the repository has.

    python tools/check_links.py

It exists because of a link that was never broken. docs/adr/0016 was written, referenced from
three commit messages and from the README, and never `git add`ed. Nothing noticed: a markdown
file does not compile and is not tested, and the link check that was run at the time resolved
every target against the working tree -- where the file was sitting, untracked. It passed. It
would have passed on a machine where the file had never existed too, because it was never asked
that question.

So this asks git instead of the filesystem. A file present on disk and absent from the
repository is the failure this is for, and it is reported differently from a path that is
simply wrong, because those two are fixed by different commands.

What it looks at, in files git is tracking:

  * markdown links and images, `[text](path)` and `![alt](path)`
  * `src=` and `srcset=` inside HTML, which the README's <picture> block uses for the two
    architecture diagrams -- and which the earlier ad-hoc check did not look at at all

What it deliberately does not look at: anchors within a document, and anything with a scheme.
Checking that `#section-name` resolves means parsing headings and slugifying them the way
GitHub does, which is a second implementation of somebody else's rule; and a http link is not
this repository's to keep working.
"""

from __future__ import annotations

import pathlib
import posixpath
import re
import subprocess
import sys
import urllib.parse

# `](path)` with the text part already consumed, and `src="path"` / `srcset="path"`. The
# markdown pattern stops at the first `)` or whitespace, which is what markdown itself does
# unless the target is in angle brackets -- a form this repository does not use.
MARKDOWN = re.compile(r"\]\(\s*<?([^)\s>]+)>?\s*(?:\"[^\"]*\")?\s*\)")
HTML = re.compile(r"(?:src|srcset)\s*=\s*[\"']([^\"']+)[\"']")

SKIP_SCHEME = re.compile(r"^(?:[a-z][a-z0-9+.-]*:|//|#)", re.IGNORECASE)


def tracked_files() -> set[str]:
    """Every path git knows about, as posix strings relative to the repository root."""
    out = subprocess.run(["git", "ls-files", "-z"], capture_output=True, check=True)
    return {name for name in out.stdout.decode("utf-8").split("\0") if name}


def repository_root() -> pathlib.Path:
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, check=True)
    return pathlib.Path(out.stdout.decode("utf-8").strip())


def targets(text: str) -> list[tuple[int, str]]:
    """Every internal reference in one document, with the line it is on."""
    found: list[tuple[int, str]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        for pattern in (MARKDOWN, HTML):
            for match in pattern.finditer(line):
                # srcset may carry a descriptor list; this repository uses one candidate, and
                # taking the first is right for the shape it does use.
                target = match.group(1).split(",")[0].strip().split(" ")[0]
                if target and not SKIP_SCHEME.match(target):
                    found.append((number, target))
    return found


def main() -> int:
    root = repository_root()
    tracked = tracked_files()
    directories = {parent for name in tracked for parent in pathlib.PurePosixPath(name).parents}

    documents = sorted(name for name in tracked if name.endswith(".md"))
    problems: list[str] = []
    checked = 0

    for document in documents:
        path = root / document
        here = pathlib.PurePosixPath(document).parent
        for number, target in targets(path.read_text(encoding="utf-8")):
            checked += 1
            # The fragment is somebody else's problem; the file part is this one's.
            without_fragment = urllib.parse.unquote(target.split("#", 1)[0])
            if not without_fragment:
                continue

            if without_fragment.startswith("/"):
                resolved = pathlib.PurePosixPath(without_fragment.lstrip("/"))
            else:
                resolved = pathlib.PurePosixPath(
                    posixpath.normpath(str(here / without_fragment)))

            name = str(resolved)
            if name in tracked or resolved in directories:
                continue

            # The distinction this whole file exists for.
            on_disk = (root / name).exists()
            why = ("exists on disk but is not tracked -- git add it"
                   if on_disk else "no such file in this repository")
            problems.append(f"{document}:{number}: {target}\n    {why}")

    for problem in problems:
        print(problem, file=sys.stderr)

    print(f"check_links: {checked} internal link(s) in {len(documents)} tracked document(s), "
          f"{len(problems)} broken")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
