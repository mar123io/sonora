#!/usr/bin/env python3
"""Receives the minidumps Crashpad posts, and writes them to a folder.

This is the endpoint installer/crash_reporter.cfg.in points at, and it is a script rather than a
service on purpose: ADR 0009 decided this project does not stand up services it cannot promise
to keep running, and a crash endpoint is the clearest example -- every installation would post
to it forever, including the ones from three years ago.

So what this is for is the loop that actually has to work: crash, handler, upload, a file on
disk, a symbolised stack. Run it, crash a debug build, and read the trace.

    python tools/crash_receiver.py --dir build/crashes
    # ...then, in another window, a debug Sonora with --simulate-crash=renderer
    ./tools/symbolise.ps1 -Dump build/crashes/<name>.dmp -Symbols build/win-release

What Crashpad sends is a multipart/form-data POST: one part per crash key (the five in
crash_reporter.cfg, plus the ones Chromium sets itself) and one file part holding the minidump.
Both halves are saved -- the keys are usually what tells you which of two identical-looking
stacks you are reading.

It answers with a plain-text id, which is what Crashpad expects and stores; anything else and
the dump is retried and then dropped.
"""

import argparse
import datetime
import email.parser
import email.policy
import gzip
import http.server
import json
import pathlib
import sys

DIRECTORY = pathlib.Path("crashes")

# A minidump of this process is a few megabytes. Anything much larger is not one, and reading
# it into memory because a header said so is how a fifty-line script becomes a denial of
# service against the person debugging with it.
MAX_BODY = 64 * 1024 * 1024


class Handler(http.server.BaseHTTPRequestHandler):
    """One POST per crash. Nothing here is a general-purpose HTTP server."""

    # Quiet by default: the interesting line is the one this prints itself.
    def log_message(self, fmt, *args):  # noqa: A003 - the base class names it
        pass

    def refuse(self, code, why):
        """Says no, out loud.

        Out loud is the whole point, and it was learnt the hard way: the first version of
        this script answered a POST it could not parse with send_error and printed nothing,
        because log_message above is silenced. Crashpad tried twice, was refused twice, and
        applied its backoff -- and from this window a receiver actively rejecting dumps looked
        exactly like a receiver nobody was uploading to.

        Those are the two states this whole part of the project exists to tell apart. A
        diagnostic tool that confuses them is worse than no diagnostic tool, because it is
        believed.
        """
        print(f"refused {code}: {why}")
        for header in ("Content-Type", "Content-Encoding", "Content-Length", "Transfer-Encoding"):
            print(f"    {header}: {self.headers.get(header)}")
        self.send_error(code, why)

    def read_chunked(self):
        """Reads a chunked request body. Returns (body, None) or (None, what went wrong).

        This exists because of the second thing the loud refusal above caught: Crashpad sends
        **no Content-Length at all**. It compresses as it writes, so it cannot know the length
        in advance, and the body arrives as Transfer-Encoding: chunked.

        http.server does not decode that -- BaseHTTPRequestHandler hands over a socket
        positioned at the first chunk header and nothing else -- which is why a fifty-line
        "receive a POST" is not enough for a POST from a real client.
        """
        body = bytearray()
        while True:
            header = self.rfile.readline(1024).strip()
            if b";" in header:  # chunk extensions; nobody sends them, and ignoring them is legal
                header = header.split(b";", 1)[0].strip()
            try:
                size = int(header, 16)
            except ValueError:
                return None, f"a chunk header that is not hexadecimal: {header!r}"
            if size == 0:
                break
            if len(body) + size > MAX_BODY:
                return None, "chunks adding up to more than a minidump"
            remaining = size
            while remaining > 0:
                piece = self.rfile.read(remaining)
                if not piece:
                    return None, f"the connection ended {remaining} bytes into a chunk"
                body += piece
                remaining -= len(piece)
            self.rfile.read(2)  # the CRLF that closes the chunk
        # Trailers, if any, then the blank line that ends the body. Read and discarded: a
        # trailer here would be a surprise, and leaving it in the socket would make the next
        # request on this connection unparseable.
        while True:
            line = self.rfile.readline(1024)
            if line in (b"\r\n", b"\n", b""):
                break
        return bytes(body), None

    def do_GET(self):  # noqa: N802 - the base class names it
        # Because the first thing anybody does with a URL is paste it into a browser, and the
        # default answer -- "501 Unsupported method ('GET')" -- is a true statement about the
        # wrong question. It says nothing about whether this is the right process, listening on
        # the right port, writing to the folder you think it is.
        received = len(list(DIRECTORY.glob("*.dmp"))) if DIRECTORY.is_dir() else 0
        body = (
            "Sonora crash receiver\n"
            "\n"
            f"writing to   {DIRECTORY.resolve()}\n"
            f"received     {received} dump(s)\n"
            "\n"
            "Only POSTs from Crashpad do anything here. That you can read this means the port\n"
            "is open and the process is alive, which is the half a browser can check.\n"
        )
        encoded = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_POST(self):  # noqa: N802 - the base class names it
        content_type = self.headers.get("Content-Type", "")
        if not content_type.startswith("multipart/form-data"):
            self.refuse(400, "expected multipart/form-data")
            return

        # Two framings. The one a browser uses says how long the body is; the one Crashpad
        # uses does not, because it is compressing as it sends.
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").strip().lower():
            body, problem = self.read_chunked()
            if body is None:
                self.refuse(400, problem)
                return
        else:
            length = int(self.headers.get("Content-Length") or 0)
            if length <= 0 or length > MAX_BODY:
                self.refuse(400, "no body, and no Transfer-Encoding either")
                return
            body = self.rfile.read(length)

        # Crashpad compresses the body and says so in a header, and this is the ordinary case
        # rather than an exotic one: the handler gzips unless it was started with
        # --no-upload-gzip, and Chromium does not pass that. So a receiver that ignores this
        # header does not receive dumps at all -- it hands a gzip stream to a MIME parser, gets
        # no parts, and refuses a perfectly good crash report.
        encoding = (self.headers.get("Content-Encoding") or "identity").strip().lower()
        if encoding == "gzip":
            try:
                body = gzip.decompress(body)
            except OSError as error:
                self.refuse(400, f"the header says gzip and the body is not: {error}")
                return
        elif encoding != "identity":
            self.refuse(400, f"unsupported Content-Encoding: {encoding}")
            return

        # email.parser and not cgi.FieldStorage: cgi was removed from the standard library in
        # Python 3.13, and a diagnostic script that stops running on a new Python is a
        # diagnostic script that is not there on the day it is needed. This is the documented
        # replacement, and it wants the Content-Type as a header rather than as an argument.
        preamble = f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n"
        message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
            preamble.encode("ascii") + body
        )
        if not message.is_multipart():
            self.refuse(400, "multipart/form-data with no parts")
            return

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        annotations = {}
        dumps = []
        for part in message.iter_parts():
            filename = part.get_filename()
            name = part.get_param("name", header="content-disposition") or filename or "unnamed"
            payload = part.get_payload(decode=True) or b""
            if filename:
                dumps.append((filename, payload))
            else:
                annotations[str(name)] = payload.decode("utf-8", errors="replace")

        DIRECTORY.mkdir(parents=True, exist_ok=True)
        written = []
        for index, (filename, data) in enumerate(dumps):
            suffix = pathlib.Path(filename or "crash.dmp").suffix or ".dmp"
            target = DIRECTORY / f"{stamp}{'' if index == 0 else f'-{index}'}{suffix}"
            target.write_bytes(data)
            written.append(target)

        # The annotations beside the dump and not inside it: the five keys from
        # crash_reporter.cfg are usually what tells two identical-looking stacks apart, and a
        # .json next to the .dmp is the shape every tool that reads either one expects.
        (DIRECTORY / f"{stamp}.json").write_text(
            json.dumps(annotations, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

        interesting = {
            key: annotations[key] for key in sorted(annotations) if key.startswith("sonora_")
        }
        print(f"{stamp}: {len(written)} dump(s), {len(annotations)} annotation(s)")
        for key, value in interesting.items():
            print(f"    {key} = {value}")
        for target in written:
            print(f"    -> {target} ({target.stat().st_size} bytes)")
        if not written:
            print("    (no dump in this POST; annotations only)")

        # Crashpad stores whatever comes back as the report id, and treats a non-2xx as a
        # failure worth retrying. A plain identifier is what a real collector answers.
        answer = stamp.encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(answer)))
        self.end_headers()
        self.wfile.write(answer)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=pathlib.Path, default=pathlib.Path("build/crashes"))
    parser.add_argument("--port", type=int, default=9911)
    # 127.0.0.1 and not 0.0.0.0, and not configurable: this receives minidumps, which are
    # memory. Binding it to a network is a decision nobody should make by typing a flag.
    arguments = parser.parse_args()

    global DIRECTORY
    DIRECTORY = arguments.dir

    server = http.server.ThreadingHTTPServer(("127.0.0.1", arguments.port), Handler)
    print(f"crash_receiver: listening on http://127.0.0.1:{arguments.port}/crashes")
    print(f"crash_receiver: writing to {DIRECTORY}")
    print("crash_receiver: this is a diagnostic tool, not a service. Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\ncrash_receiver: stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
