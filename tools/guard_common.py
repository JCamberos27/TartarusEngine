"""Shared parsing for the tools/check_*.py CI guards (allow-list files, C++ comment stripping)."""
import re
import sys

# A string or char literal (kept), or a // or /* */ comment (blanked). Literals are matched first so a "//" inside
# a string isn't taken for a comment.
_CPP_TOKEN_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.DOTALL)


def strip_cpp_comments(text: str) -> str:
    """The text with every comment replaced by blanks; newlines are kept, so line numbers still match."""
    def blank(m: re.Match) -> str:
        s = m.group(0)
        return s if s[0] in "\"'" else re.sub(r"[^\n]", " ", s)
    return _CPP_TOKEN_RE.sub(blank, text)


def parse_allowlist(text: str) -> dict[str, str]:
    """`Name = reason` lines; blank lines and whole-line # comments skipped ('#' is legal inside a reason)."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            sys.exit(f"allowlist: malformed line (no '='): {line!r}")
        name, reason = (s.strip() for s in line.split("=", 1))
        if not reason:
            sys.exit(f"allowlist: {name} has no reason")
        out[name] = reason
    return out
