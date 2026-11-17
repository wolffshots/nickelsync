#!/usr/bin/env python3
"""Mock of the Hardcover API for the NickelSync sim.

    python3 hcmock.py [port]     (default port 5931, the port in sim/Makefile)

It serves the endpoints that src/hardcover.cc uses: the OAuth device flow, the
token refresh, the revoke, and the 4 GraphQL queries. The data is sample data.

Books:
    ISBN 9780000000001 -> book 101, in the library, 8 journal entries
    ISBN 9780000000002 -> book 102, not in the library, no journal entries
    no ISBN            -> not found; the search finds book 101 and others

Tokens: the access token hc_at_sim is valid. The refresh tokens hc_rt_sim and
hc_rt_old are valid once each, as Hardcover rotates them. The first poll of a
device code is pending, the second gives tokens.
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

CLIENT_ID = "nickelsync-sim"
USER_ID = 7

access_tokens = {"hc_at_sim"}
refresh_tokens = {"hc_rt_sim", "hc_rt_old"}
device_polls = {}
counter = [0]

EDITIONS = {"9780000000001": 101, "9780000000002": 102}

TITLES = {
    101: "The Lighthouse Keeper",
    102: "Quiet Harbors",
    103: "The Lighthouse Keeper's Daughter",
}

USER_BOOKS = {
    101: {
        "updated_at": "2026-11-15T09:12:00+00:00",
        "user_book_status": {"status": "Currently Reading"},
        "user_book_reads": [
            {"progress": 42.37, "progress_pages": 160, "started_at": "2026-11-02", "finished_at": None}
        ],
    },
    103: {
        "updated_at": "2026-10-20T18:00:00+00:00",
        "user_book_status": {"status": "Want to Read"},
        "user_book_reads": [],
    },
}

LONG_QUOTE = (
    "Every night the keeper climbed the ninety steps and counted them aloud, "
    "not because he forgot the number, but because the counting was the only "
    "conversation the tower allowed. The storm took the lower window in the "
    "third winter, and he left the hole open so that the wind could speak too. "
    "By spring he knew which gusts came from the north cape and which came from "
    "the open sea, and he wrote them down in the margin of the log like names "
    "of old friends who visited without warning and left without goodbye."
)

JOURNAL = {
    101: [
        {"event": "quote", "entry": "The light does not ask the sea for permission.",
         "action_at": "2026-11-15T09:12:00+00:00", "metadata": {}},
        {"event": "progress_updated", "entry": None, "action_at": "2026-11-14T21:40:00+00:00",
         "metadata": {"progress_was": 30, "progress": 42}},
        {"event": "note", "entry": "Chapter 9 changes how I read the log of the first keeper.",
         "action_at": "2026-11-12T20:05:00+00:00", "metadata": {}},
        {"event": "quote", "entry": LONG_QUOTE, "action_at": "2026-11-10T22:30:00+00:00", "metadata": {}},
        {"event": "rated", "entry": None, "action_at": "2026-11-08T10:00:00+00:00", "metadata": {"rating": "4.5"}},
        {"event": "reviewed", "entry": None, "action_at": "2026-11-05T19:00:00+00:00",
         "metadata": {"review": [
             {"type": "paragraph", "children": [{"text": "A slow, careful book."}]},
             {"type": "paragraph", "children": [{"text": "The ending works."}]},
         ]}},
        {"event": "status_currently_reading", "entry": None, "action_at": "2026-11-02T08:00:00+00:00",
         "metadata": {}},
        {"event": "status_want_to_read", "entry": None, "action_at": "2026-11-01T12:00:00+00:00",
         "metadata": {}},
    ],
}

SEARCH_DOCS = [
    {"id": "101", "title": "The Lighthouse Keeper", "author_names": ["Mara Quill"], "release_year": 2019},
    {"id": "103", "title": "The Lighthouse Keeper's Daughter", "author_names": ["J. Pell"], "release_year": 2021},
    {"id": "201", "title": "Keeper of the Light", "author_names": ["A. Morrow"], "release_year": 2008},
    {"id": "202", "title": "Lighthouse Stories", "contributions": [
        {"contribution": None, "author": {"name": "Various"}}], "release_year": 1999},
    {"id": "203", "title": "The Keeper", "author_names": ["R. Stone"], "release_year": 2015},
    {"id": "204", "title": "Lights at Sea", "author_names": ["M. Quill", "T. Quill"], "release_year": 2023},
    {"id": "205", "title": "Harbor Lights", "author_names": ["S. Vale"], "release_year": 2012},
    {"id": "206", "title": "The Last Keeper", "author_names": ["D. Rowe"], "release_year": 2017},
    {"id": "207", "title": "Night Watch at the Cape", "author_names": ["E. Lind"], "release_year": 2020},
]


def new_tokens():
    counter[0] += 1
    at, rt = "hc_at_%d" % counter[0], "hc_rt_%d" % counter[0]
    access_tokens.add(at)
    refresh_tokens.add(rt)
    return {"access_token": at, "refresh_token": rt, "token_type": "Bearer", "expires_in": 604800,
            "scope": "read:me:content read:library read:journal read:catalog"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, status, obj, note=""):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        sys.stderr.write("hcmock: %s %s -> %d %s\n" % (self.path, note, status, obj.get("error", "")))

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/oauth2/"):
            form = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            if form.get("client_id") != CLIENT_ID:
                return self.send(401, {"error": "invalid_client"})
            return getattr(self, "oauth_" + self.path[len("/oauth2/"):], self.not_found)(form)
        if self.path == "/v1/graphql":
            return self.graphql(raw)
        self.not_found(None)

    def not_found(self, _):
        self.send(404, {"error": "not found"})

    def oauth_device(self, form):
        scopes = set(form.get("scope", "").split())
        if any(not s.startswith("read:") for s in scopes):
            return self.send(400, {"error": "invalid_scope"})
        counter[0] += 1
        code = "dev-%d" % counter[0]
        device_polls[code] = 0
        self.send(200, {"device_code": code, "user_code": "NKSY-2026",
                        "verification_uri": "https://hardcover.app/link",
                        "verification_uri_complete": "https://hardcover.app/link?code=NKSY-2026",
                        "expires_in": 600, "interval": 1}, "scope=" + " ".join(sorted(scopes)))

    def oauth_token(self, form):
        grant = form.get("grant_type")
        if grant == "urn:ietf:params:oauth:grant-type:device_code":
            code = form.get("device_code")
            if code not in device_polls:
                return self.send(400, {"error": "expired_token"})
            device_polls[code] += 1
            if device_polls[code] < 2:
                return self.send(400, {"error": "authorization_pending"})
            del device_polls[code]
            return self.send(200, new_tokens(), "device")
        if grant == "refresh_token":
            rt = form.get("refresh_token")
            if rt not in refresh_tokens:
                return self.send(400, {"error": "invalid_grant"})
            refresh_tokens.discard(rt)
            return self.send(200, new_tokens(), "refresh")
        self.send(400, {"error": "unsupported_grant_type"})

    def oauth_revoke(self, form):
        refresh_tokens.discard(form.get("token"))
        self.send(200, {})

    def graphql(self, raw):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or auth[7:] not in access_tokens:
            return self.send(401, {"error": "invalid_token"})
        req = json.loads(raw)
        v = req.get("variables", {})
        m = re.search(r"query (\w+)", req.get("query", ""))
        op = m.group(1) if m else ""
        if op == "NickelSyncEdition":
            ids = [EDITIONS[i] for i in v.get("isbn", []) if i in EDITIONS]
            data = {"editions": [{"book_id": ids[0]}] if ids else []}
        elif op == "NickelSyncStatus":
            book = v.get("book")
            ub = USER_BOOKS.get(book)
            data = {"me": [{"id": USER_ID, "user_books": [ub] if ub else []}],
                    "books_by_pk": {"title": TITLES.get(book, "Book %s" % book)}}
        elif op == "NickelSyncJournal":
            entries = JOURNAL.get(v.get("book"), []) if v.get("user") == USER_ID else []
            off, lim = v.get("offset", 0), v.get("limit", 10)
            data = {"reading_journals": [dict(e, id=1000 + i) for i, e in enumerate(entries)][off:off + lim]}
        elif op == "NickelSyncSearch":
            page, per = v.get("page", 1), v.get("per_page", v.get("limit", 6))
            hits = [{"document": d} for d in SEARCH_DOCS[(page - 1) * per:page * per]]
            data = {"search": {"results": {"hits": hits, "found": len(SEARCH_DOCS), "page": page}}}
            op += " query=%r" % v.get("query")
        else:
            return self.send(200, {"errors": [{"message": "unknown query " + op}]})
        self.send(200, {"data": data}, op)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5931
    sys.stderr.write("hcmock: listening on 127.0.0.1:%d\n" % port)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
