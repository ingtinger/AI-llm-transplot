#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Enforce the boundary between the shared request lifecycle and its two backends,
and the repository's naming and documentation conventions.

The lifecycle is written once, in gateway/src/core/, against IoOps; each backend
(gateway/src/io/) owns its loop and its syscalls and nothing else. See DESIGN.md
"Naming conventions" and GATEWAY-INTERNALS.md section 11.

  1. Includes: core/ never includes <sys/epoll.h>, <linux/io_uring.h>,
     net/uring.hpp or io/*. A backend header in core is how backend code leaks back in.
  2. Syscalls: core/ never calls ::read, ::write, ::send, ::accept, ::close or
     epoll_ctl. The lifecycle reaches a socket only through IoOps.
  3. Namespace: a header's namespace mirrors its directory.
  4. LATENCY.md: every timing stamp is attributed to a method that assigns it.
  5. Casing: kPascalCase constants, PascalCase types, ALL_CAPS only for macros.
  5b. Backends stay apart: io/epoll* never mentions io_uring and io/uring* never
     mentions epoll, in code or comments. Calling one backend from the other is
     already a compile error; this keeps either from depending on the other's rules.
  6. Release version agrees with CHANGELOG.md; comment density under the limit.

Every boundary rule is run against planted violations first, so a rule that stops
matching fails the run instead of passing everything. Pure stdlib, no build required.
Exits non-zero on any violation.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "gateway" / "src"
# The classes whose methods are the gateway: the shared Engine and the two backends.
CLASSES = ("Engine", "EpollIo", "UringIo")


def strip_comments(text):
    """Blank out comments while preserving line numbering."""
    out = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", out)


DENSITY_LIMIT = 0.8  # fails the check
DENSITY_WARN = 0.7   # warns, so a file is trimmed before it reaches the limit


def comment_density(text):
    """(comment_lines, code_lines). Blank lines count as neither; a line holding
    only a comment (or part of a block comment) is a comment line; a code line with
    a trailing comment is a code line."""
    com = code = 0
    in_block = False
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        if in_block:
            com += 1
            in_block = "*/" not in s
        elif s.startswith("//"):
            com += 1
        elif s.startswith("/*"):
            com += 1
            in_block = "*/" not in s
        else:
            code += 1
    return com, code


# ------------------------------------------------------------------ boundary rules
CORE_INCLUDES = re.compile(r'^\s*#\s*include\s*[<"](sys/epoll\.h|linux/io_uring\.h|net/uring\.hpp|io/[^">]*)[">]', re.M)
CORE_SYSCALLS = re.compile(r"(::(read|write|send|accept4?|close)\s*\(|\bepoll_ctl\s*\()")
MENTIONS_URING = re.compile(r"(?i)\b(io_)?uring|IORING")
MENTIONS_EPOLL = re.compile(r"(?i)\bepoll")


def boundary_violations(rel, text):
    """Rules 1, 2 and 5b for one file, `rel` relative to gateway/src."""
    out = []
    code = strip_comments(text)
    if rel.startswith("core/"):
        for m in CORE_INCLUDES.finditer(code):
            n = code[: m.start()].count("\n") + 1
            out.append(f"{rel}:{n}: BOUNDARY: core includes `{m.group(1)}`; only io/ may")
        for m in CORE_SYSCALLS.finditer(code):
            n = code[: m.start()].count("\n") + 1
            out.append(f"{rel}:{n}: BOUNDARY: core calls `{m.group(1).strip('( ')}`; "
                       f"reach the socket through IoOps")
    name = rel.rsplit("/", 1)[-1]
    if rel.startswith("io/epoll"):
        other = MENTIONS_URING
    elif rel.startswith("io/uring"):
        other = MENTIONS_EPOLL
    else:
        other = None
    if other:
        for m in other.finditer(text):
            n = text[: m.start()].count("\n") + 1
            out.append(f"{rel}:{n}: BOUNDARY: {name} mentions `{m.group(0)}`, the other backend")
    return out


# Each rule must catch its plant, or the run fails: a rule that silently stops
# matching would otherwise report a clean tree forever.
PLANTS = [
    ("core/x.cpp", '#include <sys/epoll.h>\n'),
    ("core/x.cpp", '#include "net/uring.hpp"\n'),
    ("core/x.hpp", '#include "io/epoll_io.hpp"\n'),
    ("core/x.cpp", 'void f(int fd) { ::close(fd); }\n'),
    ("core/x.cpp", 'void f(int fd) { (void)::write(fd, "", 0); }\n'),
    ("core/x.cpp", 'void f(int e, int fd) { epoll_ctl(e, 1, fd, nullptr); }\n'),
    ("io/epoll_io.cpp", '// unlike io_uring, a send completes here\n'),
    ("io/uring_io.cpp", 'int x = EPOLLIN;\n'),
]
CLEAN = [
    ("core/x.cpp", '#include "core/io.hpp"\nvoid f(Conn& c) { _ops->close(c); }\n// io_uring and epoll\n'),
    ("io/epoll_io.cpp", '// what is left during a pause\n'),
]


def self_test():
    failures = []
    for rel, text in PLANTS:
        if not boundary_violations(rel, text):
            failures.append(f"checker: the boundary rules missed a planted violation in {rel}: {text!r}")
    for rel, text in CLEAN:
        if boundary_violations(rel, text):
            failures.append(f"checker: the boundary rules flagged clean code in {rel}: {text!r}")
    return failures


def method_spans(text):
    """{"Class::name": (start_line, first_line_idx, last_line_idx)} for the gateway's
    classes, brace-matched so a free function between two methods is not charged to
    the one before it."""
    spans = {}
    pat = r"^[ \t]*(?:[\w:<>,\*&\s]+?)\b(%s)::(\w+)\s*\(" % "|".join(CLASSES)
    for m in re.finditer(pat, text, re.M):
        open_brace = text.find("{", m.end() - 1)
        if open_brace < 0:
            continue
        depth, i = 0, open_brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans[f"{m.group(1)}::{m.group(2)}"] = (
            text[: m.start()].count("\n") + 1,
            text[:open_brace].count("\n"),
            text[:i].count("\n"),
        )
    return spans


def main():
    failures = self_test()

    # ---------------------------------------------------------------- 1, 2, 5b
    sources = sorted(SRC.rglob("*.cpp")) + sorted(SRC.rglob("*.hpp"))
    n_core = n_io = 0
    for f in sources:
        rel = f.relative_to(SRC).as_posix()
        n_core += rel.startswith("core/")
        n_io += rel.startswith("io/")
        failures += [f"gateway/src/{v}" for v in boundary_violations(rel, f.read_text(encoding="utf-8"))]
    if n_core < 10 or n_io < 4:
        print(f"error: the boundary rules saw {n_core} core and {n_io} io files; the tree "
              f"or this check moved.", file=sys.stderr)
        return 2

    # The gateway's methods, comment-stripped, for the stamp attributions below.
    parts = [strip_comments(f.read_text(encoding="utf-8")) for f in sorted(SRC.rglob("*.cpp"))]
    text = "\n".join(parts)
    spans = method_spans(text)
    if len(spans) < 60:
        print(f"error: only {len(spans)} gateway methods parsed: the extractor is broken, "
              f"not the code. Refusing to report a clean run.", file=sys.stderr)
        return 2
    by_bare = {}
    for full in spans:
        by_bare.setdefault(full.split("::")[1], []).append(full)

    # ---------------------------------------------------------------- 3
    for header in sorted(ROOT.glob("*/include/**/*.hpp")):
        module = header.relative_to(ROOT).parts[0]
        body = header.read_text()
        m = re.search(r"^namespace\s+([\w:]+)", body, re.M)
        if not m:
            continue
        ns = m.group(1)
        ns_line = body[: m.start()].count("\n") + 1
        # gateway/ is the top-level namespace `llmbridge` by design; everything
        # else must contain its directory as a namespace segment.
        if module == "gateway":
            continue
        if module not in ns.split("::"):
            failures.append(
                f"{header.relative_to(ROOT)}:{ns_line}: NAMESPACE: `{ns}` does not mirror "
                f"its directory; expected a `{module}` segment (e.g. llmbridge::{module}::...)")

    # ---------------------------------------------------------------- 4
    # LATENCY.md section 2 attributes every timing stamp to the function that
    # assigns it. Verify each attribution still holds: the named method must exist
    # and must actually assign that stamp. Deliberately not line numbers -- those
    # rot on any edit above them, so every commit would have to re-check a doc, and
    # a reference that needs re-checking every commit is one nobody re-checks.
    # Function names move only when someone renames a function, which is exactly
    # when this table should be revisited anyway.
    latency = ROOT / "LATENCY.md"
    if latency.exists():
        doc = latency.read_text()
        lines = text.splitlines()
        row = re.compile(r"^\|\s*\*\*(t\d)\*\*\s*\|\s*`(\w+)`\s*\|.*\|([^|]*)\|\s*$", re.M)
        seen = 0
        for m in row.finditer(doc):
            stamp, ident, sites = m.group(1), m.group(2), m.group(3)
            doc_line = doc[: m.start()].count("\n") + 1
            fns = re.findall(r"`([\w:]+)`", sites)
            if not fns:
                failures.append(f"LATENCY.md:{doc_line}: STAMP: {stamp} (`{ident}`) names "
                                f"no assigning function")
                continue
            seen += 1
            for f in fns:
                # `Class::method`, or a bare name only one class defines.
                full = [f] if f in spans else by_bare.get(f, [])
                if len(full) != 1:
                    failures.append(
                        f"LATENCY.md:{doc_line}: STAMP: {stamp} cites `{f}()`, which is "
                        f"{'ambiguous' if full else 'not a gateway method (renamed or removed?)'}")
                    continue
                _, lo, hi = spans[full[0]]
                body = "\n".join(lines[lo:hi + 1])
                if not re.search(rf"\b{ident}\s*=", body):
                    failures.append(
                        f"LATENCY.md:{doc_line}: STAMP: {stamp} says `{f}()` assigns "
                        f"`{ident}`, but it does not")
        if seen < 7:
            failures.append(f"LATENCY.md: STAMP: parsed only {seen} of 7 stamp rows; "
                            f"the table or this check is broken")

    # ---------------------------------------------------------------- 5
    # Identifier casing (DESIGN.md "Naming conventions"). Documented and followed
    # by hand until now, which by this project's own standard means unenforced.
    #
    #   kPascalCase  compile-time constants
    #   PascalCase   types and enum values
    #   ALL_CAPS     preprocessor macros only
    #
    # The last one is the rule with teeth. C++ constants obey scope, so they do
    # not need the shouting that warns about a macro; and a constant in ALL_CAPS
    # collides with any system header that defines the same name (ERROR, min and
    # max are all real examples). Reserving the shape for macros keeps that
    # collision impossible instead of unlikely.
    n_consts = n_types = 0
    src = []
    for d in ("gateway", "net", "provider", "app"):
        src += sorted((ROOT / d).rglob("*.hpp")) + sorted((ROOT / d).rglob("*.cpp"))

    # `constexpr <type> NAME =` / `[` / `{`, but not `constexpr <type> name(` which
    # is a constexpr function and correctly snake_case.
    const_re = re.compile(r"\bconstexpr\b[\w:<>,\s\*&]*?\b(\w+)\s*(?:=|\[|\{)")
    type_re = re.compile(r"^\s*(?:struct|class)\s+(\w+)\s*(?:[:{]|$)", re.M)
    for f in src:
        rel = f.relative_to(ROOT)
        for n, line in enumerate(f.read_text().splitlines(), 1):
            code = line.split("//", 1)[0]
            if "constexpr" in code:
                for m in const_re.finditer(code):
                    name = m.group(1)
                    if name in ("constexpr", "static", "inline", "auto"):
                        continue
                    n_consts += 1
                    if re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                        failures.append(
                            f"{rel}:{n}: CASING - constant `{name}` is ALL_CAPS, which is "
                            f"reserved for macros; use k{name.title().replace('_','')}")
                    elif not re.fullmatch(r"k[A-Z]\w*", name):
                        failures.append(
                            f"{rel}:{n}: CASING - constant `{name}` should be kPascalCase")
            for m in type_re.finditer(code):
                name = m.group(1)
                n_types += 1
                if not re.fullmatch(r"[A-Z]\w*", name):
                    failures.append(f"{rel}:{n}: CASING - type `{name}` should be PascalCase")

    # Same guard as the method extractor: a regex that silently stops matching
    # would otherwise report a clean run over nothing at all.
    if n_consts < 30 or n_types < 20:
        print(f"error: casing check inspected only {n_consts} constants and {n_types} "
              f"types; the extractor is broken, not the code.", file=sys.stderr)
        return 2

    # ------------------------------------------------- release version agreement
    # project(version) feeds write_basic_package_version_file, so a stale value makes
    # find_package(llmbridge <ver> required) fail against a correct install. It went
    # unnoticed through the 0.11.0 tag: the consumer job calls find_package with no
    # version, so nothing compared them. Derived from the CHANGELOG, not from a
    # second hand-maintained constant, because two places to update is how it drifted.
    cml = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(\s*llmbridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", cml)
    if not m:
        failures.append("CMakeLists.txt: VERSION: no project(llmbridge VERSION x.y.z) found")
    else:
        cmake_ver = m.group(1)
        chg = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        rel = re.findall(r"^## \[([0-9]+\.[0-9]+\.[0-9]+)\]", chg, re.M)
        if not rel:
            failures.append("CHANGELOG.md: VERSION: no released `## [x.y.z]` heading found")
        elif rel[0] != cmake_ver:
            failures.append(
                f"CMakeLists.txt: VERSION: project() says {cmake_ver} but the newest "
                f"CHANGELOG entry is {rel[0]}; the installed "
                f"llmbridgeConfigVersion.cmake would lie to consumers")

    # ---------------------------------------------------------------- 6
    # Comment density: comment-only lines per code line, per source file. Long
    # rationale belongs in DESIGN.md or GATEWAY-INTERNALS.md, where it is read once,
    # not next to code that is read on every change. The license header counts.
    n_dense_warn = 0
    for f in src:
        com, code = comment_density(f.read_text(encoding="utf-8"))
        ratio = com / code if code else 0.0
        rel = f.relative_to(ROOT)
        if ratio >= DENSITY_LIMIT:
            failures.append(f"{rel}: DENSITY {ratio:.2f} ({com} comment / {code} code lines) "
                            f"is at or over {DENSITY_LIMIT}; move rationale into DESIGN.md "
                            f"or GATEWAY-INTERNALS.md")
        elif ratio >= DENSITY_WARN:
            n_dense_warn += 1
            print(f"warning: {rel}: DENSITY {ratio:.2f} ({com} comment / {code} code lines), "
                  f"fails at {DENSITY_LIMIT}", file=sys.stderr)

    # ---------------------------------------------------------------- report
    if failures:
        print(f"convention check FAILED ({len(failures)} violation"
              f"{'s' if len(failures) != 1 else ''}):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print("\nSee DESIGN.md \"Naming conventions\" and GATEWAY-INTERNALS.md 11.", file=sys.stderr)
        return 1

    print(f"convention check OK: {len(spans)} gateway methods, 0 boundary violations in "
          f"{n_core} core and {n_io} io files ({len(PLANTS)} plants caught), namespaces "
          f"mirror directories, LATENCY.md stamp refs resolve, release version agrees, "
          f"{n_consts} constants + {n_types} types correctly cased, "
          f"comment density under {DENSITY_LIMIT} everywhere ({n_dense_warn} over {DENSITY_WARN})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
