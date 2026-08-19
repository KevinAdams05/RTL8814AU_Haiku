#!/usr/bin/env python3
"""Mechanically enforce the parts of docs/STYLE_GUIDE.md that a script can.

This is a *checker*, never a reformatter. Every finding is reported for a human
to act on; nothing here rewrites a source file.

rtl8814au is a large, already-written driver (~9,500 lines of C++ in src/), so
adoption is via a **baseline**: every pre-existing finding is recorded in
scripts/style-baseline.txt and reported as "known", which lets the release gate
require zero *new* findings from day one instead of waiting on a mass
reformat. Fix a baselined finding and `--update-baseline` drops it. The
baseline only ever shrinks.

Three groups of rules here are not about formatting, and they are the reason
the script is worth running at all:

  * dprintf-prefix, raw-printf and non-ascii-str encode the logging contract
    from STYLE_GUIDE.md §19 — a kernel driver's only voice is the syslog and
    the serial console, and both want a "rtl8814au: " prefix and plain ASCII.
  * raw-usb-request, bare-new and manual-mutex encode §17 and §20.1 — the
    register-access chokepoint, the no-exceptions allocation rule, and the
    RAII locking that keeps the early-return style deadlock-free.
  * abbreviated-name encodes §3.1, the one naming rule in this project that a
    reviewer reliably forgets.

Invoke through python3: this repo keeps nothing under scripts/ executable
except the shell scripts.

Usage:
  python3 scripts/style-check.py                  # honour the baseline
  python3 scripts/style-check.py --all            # show baselined too
  python3 scripts/style-check.py --changed[=REF]  # files changed vs REF
  python3 scripts/style-check.py --update-baseline
  python3 scripts/style-check.py --list-rules
  python3 scripts/style-check.py --self-test      # verify the checker itself

Exit status is 0 when there are no new findings, 1 otherwise, so it can gate a
release directly.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO, "scripts", "style-baseline.txt")
FIXTURES = os.path.join("tests", "style")

TAB_WIDTH = 4

# §1.2. The driver holds 80 columns almost everywhere (p99 of src/*.cpp is 78
# columns), so 80 is a real target here rather than an aspiration. 100 is the
# hard stop: past it a line is not "a bit wide", it is an unwrapped dprintf.
TARGET_COLUMNS = 80
MAX_COLUMNS = 100

SOURCE_EXT = (".c", ".cpp", ".h")
SCRIPT_EXT = (".py", ".sh")
PROSE_EXT = (".md", ".svg")
TEXT_EXT = SOURCE_EXT + SCRIPT_EXT + PROSE_EXT + (".rdef",)

# Scope decides what a rule applies to, and it matters more than it looks. The
# column limits are rules about *code*: applying them to SVG (machine-emitted,
# one long path per line) or to markdown prose would bury the real findings
# under hundreds of false ones. Trailing whitespace is likewise excluded from
# markdown, where two trailing spaces are a meaningful hard line break. So is
# eol-crlf: .gitattributes pins *.cpp/*.h/*.c/*.sh to LF but deliberately
# leaves *.md native, and README.md is CRLF in the working tree because of it.
#   "text"   - every tracked text file
#   "code"   - C/C++ plus the shell and Python we wrote, plus rdef
#   "source" - C/C++ only
RULES = {
    "eol-crlf":
        ("code", "Line endings must be LF, not CRLF (.gitattributes, §2)"),
    "no-final-eol":
        ("text", "File does not end with a newline (§2)"),
    "line-over-target":
        ("code", f"Line exceeds the {TARGET_COLUMNS}-column target at tab"
            f" width {TAB_WIDTH} (§1.2)"),
    "line-too-long":
        ("code", f"Line exceeds the {MAX_COLUMNS}-column hard cap at tab width"
            f" {TAB_WIDTH} - wrap it (§1.2)"),
    "trailing-space":
        ("code", "Trailing whitespace (§2)"),
    "tab-indent":
        ("source", "Indentation must use tabs, not spaces (§2)"),
    "if-zero":
        ("source", "#if 0 block - delete it, git has the history (§18)"),
    "pragma-once":
        ("source", "Use an #ifndef header guard, not #pragma once (§15.2)"),
    "nullptr":
        ("source", "Use NULL, not nullptr (§10)"),
    "true-false":
        ("source", "Use true/false, not TRUE/FALSE (§11)"),
    "non-ascii-str":
        ("source", "Non-ASCII character in a string literal - the syslog and"
            " the serial console are ASCII (§20.4)"),
    "yoda-condition":
        ("source", "Constant on the left of a comparison (§8.1)"),
    "cast-space":
        ("source", "No whitespace after a C-style cast operator (§9.4)"),
    "pointer-space":
        ("source", "Asterisk binds to the type: 'uint8* buffer' (§10)"),
    "return-parens":
        ("source", "Do not parenthesise a return expression (§12)"),
    "todo-owner":
        ("source", "TODO must not name an author - git knows (§13)"),
    "function-brace":
        ("source", "A function's opening brace goes on its own line (§4)"),
    "function-spacing":
        ("source", "Exactly two blank lines between top-level definitions"
            " (§7)"),
    "same-line-body":
        ("source", "The body of an if/else/for/while goes on its own line,"
            " never after the condition (§4)"),
    "implicit-bitmask":
        ("source", "Bitmask test needs an explicit comparison:"
            " if ((flags & kMask) != 0) (§8.1)"),
    "goto-used":
        ("source", "No goto - use early return or a scoped helper (§8.5)"),
    "raw-printf":
        ("source", "Raw printf/fprintf - the kernel has no stdout, use"
            " dprintf() (§19)"),
    "dprintf-prefix":
        ("source", 'dprintf() must lead with RTL8814AU_DRIVER_NAME ": " so'
            " syslog lines are greppable (§19)"),
    "bare-new":
        ("source", "Kernel code is built without exceptions - use"
            " new(std::nothrow) and check for NULL (§17)"),
    "manual-mutex":
        ("source", "Use MutexLocker, not a manual mutex_lock/mutex_unlock"
            " pair (§17)"),
    "raw-usb-request":
        ("source", "USB control transfers belong in RegisterIO.cpp - go"
            " through the RTL8814AURegisterIO API (§20.1)"),
    "const-k-prefix":
        ("source", "File-scope constants take a 'k' prefix (§3)"),
    "member-f-prefix":
        ("source", "Class data members take an 'f' prefix (§3)"),
    "global-g-prefix":
        ("source", "Globals take a 'g' prefix (§3)"),
    "abbreviated-name":
        ("source", "Abbreviated identifier - spell it out (§3.1)"),
    "missing-copyright":
        ("source", "Missing MIT copyright header (§16)"),
    "header-guard":
        ("source", "Header guard missing or does not match the filename"
            " (§15.2)"),
    "include-order":
        ("source", "Includes within a group must be alphabetised (§14.1)"),
}


def describe(rule):
    return RULES[rule][1]


# Directories holding build output, the firmware blob, or the linter's own
# deliberately-broken fixtures. Only consulted by the non-git fallback, except
# FIXTURES which is always excluded from a normal run.
PRUNE_DIRS = {".git", "build", "dist", "firmware", "__pycache__"}

# §21. Register-definition headers are transcriptions of the chip's register
# map. Their identifier names mirror the reference driver's field names so the
# two can be diffed, and their tables carry an aligned trailing comment per
# line that wrapping at 80 would destroy. Formatting and correctness rules
# still apply; naming and line width do not.
REGISTER_HEADERS = {
    "src/RTL8814AU.h",
    "src/PhyRegTables.h",
}

# §3.4. WiFiIoctl.h mirrors FreeBSD's net80211 ioctl ABI byte for byte. Its
# struct field names (i_len, ik_keyix, isr_ie_off, ...) are the ABI; renaming
# them to something descriptive would be renaming somebody else's interface.
ABI_HEADERS = {
    "src/WiFiIoctl.h",
}

# §1.5. tools/ is userland C built against the public headers, not kernel C++.
# printf *is* its output channel, its functions are snake_case by C convention,
# and it has no SupportDefs.h, no dprintf, no new, and no mutexes.
C_TOOL_PREFIX = "tools/"

C_TOOL_EXEMPT = {
    "raw-printf", "dprintf-prefix", "bare-new", "manual-mutex",
    "raw-usb-request", "const-k-prefix", "global-g-prefix",
    "member-f-prefix", "abbreviated-name", "include-order", "nullptr",
}

REGISTER_HEADER_EXEMPT = {
    "line-over-target", "line-too-long", "abbreviated-name",
    "member-f-prefix", "include-order",
}

ABI_HEADER_EXEMPT = {
    "abbreviated-name", "member-f-prefix",
}

# The one file allowed to talk to the USB bus manager's send_request() hook
# directly. Everything else reaches the chip through RTL8814AURegisterIO, which
# is what makes a register trace possible from one place (§20.1).
USB_REQUEST_ALLOWED = {"src/RegisterIO.cpp"}

# Symbols the Haiku kernel add-on ABI dictates. They are file-scope
# definitions with names we do not get to choose, so the g-prefix rule has to
# step around them.
HAIKU_EXPORTED_SYMBOLS = {
    "api_version", "publish_devices", "find_device", "init_driver",
    "uninit_driver", "init_hardware", "std_ops", "usb_notify_hooks",
    "driver_name", "driver_version",
}

# §3.1. Abbreviations whose spelled-out form is unambiguous and shorter to
# read than to decode. Deliberately short: every entry here is one a reviewer
# has to remember, so the list earns its keep only if it stays memorable.
#
# Not on the list, and not flagged, because in this domain they *are* the
# spelled-out form: addr, desc (RX/TX descriptor), mac, ie (information
# element), seq, sec, cfg, pkt, chan, phy, rf, bb, efuse, dma, usb, mcu, ssid,
# bssid, gtk, ptk, pmk, ccmp, eapol, rssi, tid, aad.
ABBREVIATIONS = {
    "msg": "message",
    "idx": "index",
    "ptr": "pointer",
    "cnt": "count",
    "buf": "buffer",
    "buff": "buffer",
    "len": "length",
    "val": "value",
    "hdr": "header",
    "resp": "response",
    "err": "error",
    "sz": "size",
    "ctx": "context",
    "rslt": "result",
    "nbr": "number",
}

# A short lowercase prefix plus an underscore is how the net80211 ABI names
# its struct fields (i_len, ik_keydata, isr_ie_off, jp_ssid_len). Those names
# are not ours to change wherever they appear, not only inside WiFiIoctl.h.
ABI_FIELD = re.compile(r"^[a-z]{1,4}_[a-z]")

# Keywords that can legitimately be followed by "(...) {" on one line.
BRACE_KEYWORDS = ("if", "else", "for", "while", "switch", "do", "return",
    "class", "struct", "union", "enum", "namespace", "extern", "typedef",
    "template", "catch", "try")

CAST_TYPES = (r"u?int(?:8|16|32|64)|char|short|long|float|double|"
    r"size_t|ssize_t|off_t|status_t|addr_t|phys_addr_t|uchar|uint|ulong|"
    r"unsigned|signed|void")

# Types common enough in this driver that a following " *name" is a
# declaration rather than a multiplication.
POINTER_TYPES = (r"char|void|uint8|uint16|uint32|uint64|int8|int16|int32|"
    r"int64|size_t|off_t|status_t|addr_t|usb_module_info|net_notifications"
    r"_module_info|RTL8814AUDevice|RTL8814AURegisterIO|RTL8814AUFirmware|"
    r"RTL8814AUEfuseReader|RTL8814AUPhyConfig|RTL8814AUTxPath|RTL8814AURxPath|"
    r"RTL8814AUWiFiManager|RxFrameInfo|BssInfo|WiFiLinkState|PowerSeqCommand")

# A class data member as this codebase writes them: one tab, a type, a run of
# tabs for column alignment, then the name. Restricting the pattern to the
# tab-aligned form is what keeps it off local variables and off the POD
# hardware structs, which are `struct`s and correctly carry no f prefix.
MEMBER_DECLARATION = re.compile(
    r"^\t(?:mutable\s+)?(?:const\s+)?"
    r"([A-Za-z_][\w:]*(?:\s*[*&])?)\t+([A-Za-z_]\w*)"
    r"\s*(?:\[[^\]]*\])?\s*;")

FILE_SCOPE_DEFINITION = re.compile(
    r"^([A-Za-z_][\w:]*(?:\s*\*)?)\s+([A-Za-z_]\w*)"
    r"\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;\s*$")

FILE_SCOPE_CONSTANT = re.compile(
    r"^static\s+const\s+[A-Za-z_][\w:]*(?:\s*\*)?\s+([A-Za-z_]\w*)")

INCLUDE_LINE = re.compile(r'^#\s*include\s+[<"]([^>"]+)[>"]')

# A row of a constant table: nothing but numeric literals, commas, optional
# braces and whitespace. §21 exempts these from the column limits for the same
# reason the register headers are exempt - the value of a table is that its
# columns line up, and wrapping one row at 80 destroys that for the whole
# table. The AES S-boxes in WPA2Crypto.cpp are 16 bytes per row at 99 columns
# and are meant to be read as a 16x16 grid.
TABLE_ROW = re.compile(r"^[\s{},]*(?:0[xX][0-9a-fA-F]+|\d+)"
    r"(?:\s*,\s*(?:0[xX][0-9a-fA-F]+|\d+))+[\s{},;]*$")


def git(*arguments):
    """Run git, or return None if this is not a usable checkout.

    package/build-hpkg.sh calls this as a release gate, and a release may be
    built from an unpacked tarball with no .git, so every git use has to
    degrade rather than raise.
    """
    try:
        result = subprocess.run(["git", "-C", REPO, *arguments],
            capture_output=True, text=True)
    except OSError:
        return None
    return result.stdout if result.returncode == 0 else None


def walked_files():
    """Every text file under the repo, for use without git."""
    found = []
    for root, directories, names in os.walk(REPO):
        directories[:] = [d for d in directories if d not in PRUNE_DIRS]
        for name in names:
            if name.endswith(TEXT_EXT):
                full = os.path.join(root, name)
                found.append(os.path.relpath(full, REPO))
    return sorted(found)


def tracked_files():
    """Tracked files plus new ones not yet committed.

    Untracked-but-not-ignored files are included deliberately: a brand new
    source file is exactly the case where a style check earns its keep, and
    leaving it out would mean the gate never saw it until after it was
    committed.
    """
    out = git("ls-files")
    if out is None:
        print("style-check: not a git checkout - walking the filesystem",
            file=sys.stderr)
        names = set(walked_files())
    else:
        names = set(out.splitlines())
        new = git("ls-files", "--others", "--exclude-standard")
        if new is not None:
            names |= set(new.splitlines())
    return [f for f in sorted(names) if _checkable(f)]


def changed_files(ref):
    worktree = git("diff", "--name-only", ref)
    staged = git("diff", "--name-only", "--cached")
    if worktree is None or staged is None:
        print(f"style-check: cannot diff against '{ref}' - checking "
            "everything instead", file=sys.stderr)
        return tracked_files()
    names = set(worktree.splitlines()) | set(staged.splitlines())
    return [f for f in sorted(names) if _checkable(f)]


def _normalise(path):
    return path.replace(os.sep, "/")


def _checkable(path):
    if not path.endswith(TEXT_EXT):
        return False
    normalised = _normalise(path)
    # The fixtures are broken on purpose; checking them would always fail.
    if normalised.startswith(_normalise(FIXTURES)):
        return False
    return not any(part in PRUNE_DIRS for part in normalised.split("/"))


def exempt_rules(path):
    """Rules that do not apply to this file (§21)."""
    normalised = _normalise(path)
    if normalised.startswith(C_TOOL_PREFIX) and normalised.endswith(".c"):
        return C_TOOL_EXEMPT
    if normalised in REGISTER_HEADERS:
        return REGISTER_HEADER_EXEMPT
    if normalised in ABI_HEADERS:
        return ABI_HEADER_EXEMPT
    return frozenset()


def visual_width(line):
    width = 0
    for char in line:
        if char == "\t":
            width = (width // TAB_WIDTH + 1) * TAB_WIDTH
        else:
            width += 1
    return width


def string_literals(line):
    """Double-quoted literals on a line, escapes respected."""
    return re.findall(r'"(?:[^"\\]|\\.)*"', line)


def strip_strings(code):
    """Blank the *contents* of string and char literals, keeping the quotes.

    Every keyword and identifier rule runs against this rather than against
    the raw code. Without it the checker reads English: "starting a new one"
    in a dprintf tripped bare-new, and the format string ": ie_len=%u" put an
    abbreviated-name finding on a syslog message. Columns are preserved so
    line numbers and paren depth still line up.
    """
    return re.sub(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'',
        lambda match: '"' + " " * (len(match.group(0)) - 2) + '"', code)


def code_view(lines):
    """Blank out comments while leaving string literals and columns intact.

    Returns a list of (code, in_comment) pairs, one per input line. `code` has
    every comment character replaced by a space, so column numbers and string
    literals survive; `in_comment` is True when the line *starts* inside a
    block comment, which is what the indentation rule needs in order to leave
    the doxygen blocks alone.

    A full scanner rather than a `line.find("//")`, because this codebase
    documents its functions with multi-line `/*! ... */` blocks that quite
    reasonably contain the words TRUE, FALSE and goto while explaining what
    the reference driver did. Splitting on "//" alone made three rules fire
    entirely on prose.
    """
    result = []
    in_block = False
    for line in lines:
        started_in_block = in_block
        out = []
        index = 0
        length = len(line)
        while index < length:
            char = line[index]
            if in_block:
                if line.startswith("*/", index):
                    in_block = False
                    out.append("  ")
                    index += 2
                    continue
                out.append(" " if char != "\t" else "\t")
                index += 1
                continue
            if line.startswith("/*", index):
                in_block = True
                out.append("  ")
                index += 2
                continue
            if line.startswith("//", index):
                out.append(" " * (length - index))
                break
            if char in ('"', "'"):
                quote = char
                out.append(char)
                index += 1
                while index < length:
                    out.append(line[index])
                    if line[index] == "\\" and index + 1 < length:
                        index += 1
                        out.append(line[index])
                    elif line[index] == quote:
                        index += 1
                        break
                    index += 1
                continue
            out.append(char)
            index += 1
        result.append(("".join(out), started_in_block))
    return result


def name_segments(name):
    """Split an identifier into lowercase words: rxFrameLen -> rx frame len."""
    segments = []
    for part in re.split(r"_+", name):
        segments += [piece.lower() for piece in re.findall(
            r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+", part)]
    return segments


def expected_guard(path):
    """src/WiFiManagement.h -> RTL8814AU_WIFI_MANAGEMENT_H

    The chip name is one word, so the naive camel-to-snake split has to be
    taught two things: an uppercase run is one word (RegisterIO -> REGISTER_IO,
    not REGISTER_I_O) and a digit does not start a new one (RTL8814AU stays
    whole). "WiFi" is spelled with an inner capital everywhere in this tree but
    guards it as WIFI, so it gets an alias.
    """
    stem = os.path.splitext(os.path.basename(path))[0]
    stem = stem.replace("WiFi", "Wifi")
    spaced = re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"
        r"|(?<=[0-9])(?=[A-Z][a-z])", "_", stem)
    upper = re.sub(r"_+", "_", spaced).upper()
    if upper == "RTL8814AU":
        return "RTL8814AU_H"
    return f"RTL8814AU_{upper}_H"


def check_file(path, findings, root=REPO):
    full = os.path.join(root, path)
    try:
        raw = open(full, "rb").read()
    except OSError as error:
        print(f"style-check: cannot read {path}: {error}", file=sys.stderr)
        return

    is_source = path.endswith(SOURCE_EXT)
    is_code = is_source or path.endswith(SCRIPT_EXT) or path.endswith(".rdef")
    exempt = exempt_rules(path)

    def add(rule, line_number, detail=""):
        scope = RULES[rule][0]
        if scope == "source" and not is_source:
            return
        if scope == "code" and not is_code:
            return
        if rule in exempt:
            return
        findings.append((path, rule, line_number, detail))

    if b"\r\n" in raw:
        add("eol-crlf", 0)

    if len(raw) > 0 and not raw.endswith(b"\n"):
        add("no-final-eol", 0)

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return

    if not is_code:
        return

    lines = [line.rstrip("\r") for line in text.split("\n")]

    for number, line in enumerate(lines, 1):
        if line != line.rstrip():
            add("trailing-space", number)

        width = visual_width(line)
        if width > TARGET_COLUMNS and TABLE_ROW.match(line) is None:
            if width > MAX_COLUMNS:
                add("line-too-long", number, f"{width} columns")
            else:
                add("line-over-target", number, f"{width} columns")

    if not is_source:
        return

    view = code_view(lines)
    for number, (code, in_comment) in enumerate(view, 1):
        check_source_line(path, number, lines[number - 1], code, in_comment,
            add)

    check_copyright(text, add)
    check_structure(path, lines, view, add)
    check_includes(view, add)
    if path.endswith(".h"):
        check_header_guard(path, text, add)


def check_source_line(path, number, line, raw_code, in_comment, add):
    normalised = _normalise(path)

    for literal in string_literals(raw_code):
        if any(ord(char) > 127 for char in literal):
            add("non-ascii-str", number)
            break

    code = strip_strings(raw_code)

    # Leading spaces used as indentation. A line that opens inside a block
    # comment is doxygen body text, which this tree indents with four spaces on
    # purpose (see the `/*! ... */` blocks above every public method), and the
    # " *" continuation form is legitimate too.
    if not in_comment and re.match(r"^ +\S", line) \
            and not re.match(r"^ +\*", line):
        add("tab-indent", number)

    if re.match(r"^\s*#\s*if\s+0\b", line):
        add("if-zero", number)

    if re.match(r"^\s*#\s*pragma\s+once\b", line):
        add("pragma-once", number)

    if re.search(r"\bnullptr\b", code):
        add("nullptr", number)

    if re.search(r"\b(?:TRUE|FALSE)\b", code):
        add("true-false", number)

    if re.search(r"\bgoto\b", code):
        add("goto-used", number)

    # snprintf/vsnprintf format into a caller's buffer and are fine; printf
    # and friends write to a stdout the kernel does not have.
    if re.search(r"(?<![\w.>])(?:printf|fprintf|puts|fputs|putchar|fputc)"
            r"\s*\(", code) and not re.search(r"#\s*define", code):
        add("raw-printf", number)

    # The prefix has to be the first thing in the format string. Continuation
    # lines of an already-prefixed call are wrapped string literals, not
    # dprintf calls, so they never reach this test.
    match = re.search(r"\bdprintf\s*\(\s*(.)", code)
    if match is not None and "RTL8814AU_DRIVER_NAME" not in code \
            and match.group(1) == '"':
        add("dprintf-prefix", number)

    if re.search(r"(?<![\w:])new\s+(?!\()", code) \
            and "std::nothrow" not in code:
        add("bare-new", number)

    if re.search(r"\bmutex_(?:lock|unlock)\s*\(", code):
        add("manual-mutex", number)

    if normalised not in USB_REQUEST_ALLOWED \
            and re.search(r"\bsend_request\s*\(", code):
        add("raw-usb-request", number)

    # Yoda conditions: a literal or B_* constant on the left of == or !=.
    if re.search(r"[(&|]\s*(?:NULL|true|false|B_[A-Z0-9_]+|-?\d+)\s*[=!]=",
            code):
        add("yoda-condition", number)

    # "(uint32) value" - the space is the violation, not the cast.
    if re.search(rf"\((?:{CAST_TYPES})\s*\*?\)\s+\w", code):
        add("cast-space", number)

    if re.search(rf"(?:^\s*|[(,]\s*|\b(?:const|static)\s+)(?:{POINTER_TYPES})"
            r"\s+\*\s*\w", code):
        add("pointer-space", number)

    if re.search(r"\breturn\s*\([^;]*\)\s*;", code) \
            and not re.search(r"\breturn\s*\(.*[-+*/&|^<>!=,]", code):
        add("return-parens", number)

    # TODO(kevin) or TODO: kevin - both name a person.
    if re.search(r"\bTODO\s*\(", line) \
            or re.search(r"\bTODO:\s*kevin\b", line, re.IGNORECASE):
        add("todo-owner", number)

    # A statement parked on the same line as its condition. Anchored on the
    # closing paren of the control expression, with the paren depth counted so
    # that a wrapped condition ending in ") {" does not look like a body.
    body = same_line_body(code)
    if body is not None:
        add("same-line-body", number, body)

    if implicit_bitmask(code):
        add("implicit-bitmask", number)

    check_names(path, number, code, add)


def same_line_body(code):
    """'if (x) return y;' -> 'return y;'   'if (x) {' -> None"""
    match = re.match(r"^\s*(?:\}\s*)?(?:else\s+)?(if|for|while)\s*\(", code)
    if match is None:
        return None
    index = code.index("(", match.end(1) - 1)
    depth = 0
    for position in range(index, len(code)):
        if code[position] == "(":
            depth += 1
        elif code[position] == ")":
            depth -= 1
            if depth == 0:
                rest = code[position + 1:].strip()
                if rest in ("", "{", "\\") or rest.startswith("//"):
                    return None
                return rest[:40]
    return None


def implicit_bitmask(code):
    """if (value & kMask) with no comparison against 0 (§8.1)."""
    match = re.match(r"^\s*(?:\}\s*)?(?:else\s+)?(?:if|while)\s*\(", code)
    if match is None:
        return False
    index = code.index("(", match.end() - 1)
    depth = 0
    condition = ""
    for position in range(index, len(code)):
        char = code[position]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                condition = code[index + 1:position]
                break
    if not condition:
        return False
    # Strip nested parenthesised groups so an inner "(a & b) != 0" does not
    # make the whole condition look compared-against-nothing.
    if re.search(r"[=!<>]=|[<>]", condition):
        return False
    return re.search(r"(?<![&\s])\s*&\s*(?!&)", condition) is not None


def check_names(path, number, code, add):
    """§3 prefixes and §3.1 spelled-out names."""
    constant = FILE_SCOPE_CONSTANT.match(code)
    if constant is not None and not re.match(r"^k[A-Z0-9]",
            constant.group(1)):
        add("const-k-prefix", number, constant.group(1))

    definition = FILE_SCOPE_DEFINITION.match(code)
    if definition is not None \
            and definition.group(1) not in ("return", "static", "extern",
                "typedef", "struct", "class", "enum", "using", "const",
                "delete") \
            and definition.group(2) not in HAIKU_EXPORTED_SYMBOLS \
            and not re.match(r"^g[A-Z]", definition.group(2)):
        add("global-g-prefix", number, definition.group(2))


def check_abbreviations(path, lines, view, add):
    """One finding per abbreviated identifier per file, at first sight.

    Reporting every *use* would put 363 entries in the baseline for what is
    really 80 identifiers, and every rename would then invalidate a dozen
    baseline lines at once. First-sight-only keeps the baseline the same size
    as the actual work.
    """
    seen = set()
    for number, (raw_code, _) in enumerate(view, 1):
        code = strip_strings(raw_code)
        for match in re.finditer(r"\b[A-Za-z_]\w*\b", code):
            name = match.group(0)
            if name in seen:
                continue
            if name.isupper() or re.match(r"^k[A-Z0-9]", name) \
                    or ABI_FIELD.match(name):
                continue
            for segment in name_segments(name):
                if segment in ABBREVIATIONS:
                    seen.add(name)
                    add("abbreviated-name", number,
                        f"{name}: '{segment}' -> '{ABBREVIATIONS[segment]}'")
                    break


def check_structure(path, lines, view, add):
    """Brace placement, blank-line spacing, member prefixes."""
    check_abbreviations(path, lines, view, add)

    class_depth = None
    depth = 0
    for number, line in enumerate(lines, 1):
        code = strip_strings(view[number - 1][0])

        if class_depth is None and re.match(r"^\s*class\s+\w+", code) \
                and "{" in code:
            class_depth = depth
        elif class_depth is not None:
            member = MEMBER_DECLARATION.match(code)
            if member is not None and not member.group(2).startswith("f") \
                    and "(" not in code:
                add("member-f-prefix", number, member.group(2))

        depth += code.count("{") - code.count("}")
        if class_depth is not None and depth <= class_depth:
            class_depth = None

        check_function_brace(code, number, add)

    # Two blank lines between top-level definitions. Only a closing brace in
    # column 0 counts as the end of one, which is exactly the shape this tree
    # uses and keeps the rule off nested blocks.
    for index in range(len(lines) - 2):
        if lines[index] != "}":
            continue
        following = lines[index + 1:index + 3]
        if following == ["", ""]:
            continue
        if all(entry.strip() == "" for entry in following):
            continue
        if following[0].strip().startswith(("#endif", "}", ")", ";")):
            continue
        add("function-spacing", index + 1)


def check_function_brace(code, number, add):
    """A function definition must not open its brace on the signature line.

    Deliberately conservative: only fires on a line starting at column 0 that
    looks like a complete signature ending in '{'. Anything indented is a
    method inside a class body, a lambda, or a control block, none of which
    this rule governs.
    """
    stripped = code.rstrip()
    if not stripped.endswith("{"):
        return
    if code[:1] in ("", " ", "\t", "#", "/", "*"):
        return
    if "(" not in stripped or ")" not in stripped:
        return
    first_word = re.match(r"^(\w+)", stripped)
    if first_word is not None and first_word.group(1) in BRACE_KEYWORDS:
        return
    head = stripped.split("(")[0]
    if ":" in head and "::" not in head:
        return
    if re.search(r"=\s*\{$", stripped):
        return
    add("function-brace", number)


def check_includes(view, add):
    """Each run of consecutive #include lines must be sorted (§14.1).

    Accepts either ASCII or case-insensitive order, because the tree contains
    groups that are only sorted under one of the two (<KernelExport.h> before
    <net/if_media.h> needs ASCII; <ether_driver.h> before
    <NetworkNotifications.h> needs case-insensitive). Flagging a group takes
    both to fail, which keeps the rule at real findings.
    """
    group = []
    for number, (code, _) in enumerate(view, 1):
        match = INCLUDE_LINE.match(code)
        if match is not None:
            group.append((number, match.group(1)))
            continue
        flush_include_group(group, add)
        group = []
    flush_include_group(group, add)


def flush_include_group(group, add):
    if len(group) < 2:
        return
    names = [name for _, name in group]
    if names == sorted(names) or names == sorted(names, key=str.lower):
        return
    add("include-order", group[0][0], f"{len(names)} includes")


def check_copyright(text, add):
    """The MIT block has to be the first thing in the file (§16)."""
    head = text[:600]
    if "Copyright" not in head or "Kevin Adams" not in head \
            or "MIT License" not in head:
        add("missing-copyright", 1)


def check_header_guard(path, text, add):
    """#ifndef FOO_H / #define FOO_H / #endif  // FOO_H, matching the file."""
    guard = expected_guard(path)

    ifndef = re.search(r"^\s*#\s*ifndef\s+(\w+)\s*$", text, re.M)
    define = re.search(r"^\s*#\s*define\s+(\w+)\s*$", text, re.M)

    if ifndef is None or define is None:
        add("header-guard", 1, "no #ifndef/#define guard")
        return

    if ifndef.group(1) != guard:
        add("header-guard", text[:ifndef.start()].count("\n") + 1,
            f"expected {guard}, found {ifndef.group(1)}")
        return

    if define.group(1) != ifndef.group(1):
        add("header-guard", text[:define.start()].count("\n") + 1,
            "#define does not match #ifndef")
        return

    if re.search(rf"#\s*endif\s+(?://|/\*)\s*{re.escape(guard)}", text,
            re.M) is None:
        add("header-guard", text.count("\n") + 1,
            f"closing #endif needs a '// {guard}' comment")


def load_baseline():
    if not os.path.exists(BASELINE):
        return set()
    known = set()
    for line in open(BASELINE):
        line = line.strip()
        if line and not line.startswith("#"):
            known.add(line)
    return known


def key_of(finding):
    path, rule, line_number, _ = finding
    return f"{path}\t{rule}\t{line_number}"


SELF_TEST_EXPECTED = {
    "bad.cpp": {
        "trailing-space", "tab-indent", "if-zero", "raw-printf", "nullptr",
        "true-false", "non-ascii-str", "yoda-condition", "cast-space",
        "pointer-space", "return-parens", "todo-owner", "function-brace",
        "function-spacing", "same-line-body", "implicit-bitmask",
        "goto-used", "dprintf-prefix", "bare-new", "manual-mutex",
        "raw-usb-request", "const-k-prefix", "global-g-prefix",
        "abbreviated-name", "missing-copyright", "include-order",
        "line-over-target", "line-too-long",
    },
    "bad.h": {
        "pragma-once", "header-guard", "missing-copyright",
        "member-f-prefix",
    },
    "good.cpp": set(),
    "good.h": set(),
}

# eol-crlf and no-final-eol cannot live in a committed fixture: .gitattributes
# pins *.cpp/*.h to eol=lf, so git would repair the violation on checkout and
# the rules would ship having never matched. They get a scratch file instead.
SELF_TEST_BYTES = {
    "crlf.cpp": (b"/* Copyright 2026, Kevin Adams <kevinadams05@gmail.com>\r\n"
        b" * MIT License */\r\n", {"eol-crlf"}),
    "noeol.cpp": (b"/* Copyright 2026, Kevin Adams "
        b"<kevinadams05@gmail.com>\n * MIT License */", {"no-final-eol"}),
}


def self_test():
    """Run the checker over tests/style/ and compare against expectations.

    Without this the register-header and tools/ exemptions in particular would
    be untested logic on a path nothing exercises: a normal run over a clean
    tree reports nothing, which looks identical to a checker that matches
    nothing. The fixtures are excluded from normal runs.
    """
    directory = os.path.join(REPO, FIXTURES)
    if not os.path.isdir(directory):
        print(f"style-check: no fixtures at {FIXTURES}", file=sys.stderr)
        return 1

    failures = 0
    for name, expected in sorted(SELF_TEST_EXPECTED.items()):
        path = os.path.join(FIXTURES, name)
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"  MISSING  {path}")
            failures += 1
            continue
        failures += compare(path, expected, REPO)

    with tempfile.TemporaryDirectory(prefix="rtl8814au-style-") as scratch:
        for name, (payload, expected) in sorted(SELF_TEST_BYTES.items()):
            with open(os.path.join(scratch, name), "wb") as handle:
                handle.write(payload)
            failures += compare(name, expected, scratch)

    print()
    if failures:
        print(f"style-check: self-test FAILED ({failures} fixture(s))")
        return 1
    print("style-check: self-test passed")
    return 0


def compare(path, expected, root):
    findings = []
    check_file(path, findings, root=root)
    found = {rule for _, rule, _, _ in findings}

    missing = expected - found
    unexpected = found - expected
    if not missing and not unexpected:
        print(f"  ok       {path}  ({len(found)} rule(s) as expected)")
        return 0

    print(f"  FAIL     {path}")
    for rule in sorted(missing):
        print(f"             not detected: {rule}")
    for rule in sorted(unexpected):
        lines = sorted(n for _, r, n, _ in findings if r == rule)
        print(f"             false positive: {rule} at line(s) "
            f"{lines[:6]}")
    return 1


def main():
    parser = argparse.ArgumentParser(add_help=True,
        description="Check this repo against docs/STYLE_GUIDE.md.")
    parser.add_argument("--all", action="store_true",
        help="report baselined findings too")
    parser.add_argument("--changed", nargs="?", const="HEAD", metavar="REF",
        help="only check files changed against REF (default HEAD)")
    parser.add_argument("--update-baseline", action="store_true",
        help="rewrite the baseline from the current findings")
    parser.add_argument("--list-rules", action="store_true")
    parser.add_argument("--self-test", action="store_true",
        help="verify the checker against tests/style/ fixtures")
    arguments = parser.parse_args()

    if arguments.list_rules:
        for rule, (scope, description) in sorted(RULES.items()):
            print(f"{rule:20s} [{scope:6s}] {description}")
        return 0

    if arguments.self_test:
        return self_test()

    files = changed_files(arguments.changed) if arguments.changed \
        else tracked_files()
    if not files:
        print("style-check: nothing to check")
        return 0

    findings = []
    for path in files:
        check_file(path, findings)
    findings.sort(key=lambda finding: (finding[0], finding[2], finding[1]))

    baseline = set() if (arguments.all or arguments.update_baseline) \
        else load_baseline()

    if arguments.update_baseline:
        counts = {}
        for _, rule, _, _ in findings:
            counts[rule] = counts.get(rule, 0) + 1
        with open(BASELINE, "w") as handle:
            handle.write("# Pre-existing style findings in rtl8814au, "
                "recorded so the release gate can\n")
            handle.write("# require zero *new* findings without a mass "
                "reformat first.\n")
            handle.write("#\n")
            handle.write("# Regenerate with:  python3 scripts/style-check.py "
                "--update-baseline\n")
            handle.write("# Shrinking this file is always welcome; growing "
                "it needs a reason.\n")
            handle.write("#\n")
            handle.write("# Format: <path>\\t<rule>\\t<line>\n")
            handle.write("#\n")
            handle.write(f"# {len(findings)} finding(s) by rule:\n")
            for rule in sorted(counts, key=lambda r: (-counts[r], r)):
                handle.write(f"#   {counts[rule]:4d}  {rule}\n")
            handle.write("\n")
            for finding in findings:
                handle.write(key_of(finding) + "\n")
        print(f"style-check: baseline written with {len(findings)} finding(s)")
        return 0

    new = [f for f in findings if key_of(f) not in baseline]
    known = len(findings) - len(new)

    for path, rule, line_number, detail in new:
        where = f"{path}:{line_number}" if line_number else path
        suffix = f" ({detail})" if detail else ""
        print(f"{where}: {rule}: {describe(rule)}{suffix}")

    if new:
        print()
    print(f"style-check: {len(files)} file(s), {len(new)} new finding(s)"
        + (f", {known} baselined" if known else ""))

    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
