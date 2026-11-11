# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""NFR-SEC-03 static check: no logging call in the code that handles session
material formats a secret-looking variable, and the extractor's arguments
(which carry `--cookies <jar>`) are never logged.

Scope (every pattern must match at least one file, so a move cannot silently
empty the scan):
  Core/Account/**                                  the account component
  Clients/Sailfish/Presentation/AccountController.*  its QML view
  Core/Resolver/Infrastructure/YtDlpRunner.*       adds --cookies to the run
  Core/Worker/**                                   the warm worker that runs yt-dlp
  Shared/SailTune/Core/Accounts/**                 the pinned SailTune account code

Logging calls:
  C++     qDebug/qInfo/qWarning/qCritical/qFatal (printf and stream style, also
          the qC* category forms), printf/fprintf/fputs/puts/perror/syslog,
          std::cout/cerr/clog <<, QTextStream(stdout|stderr) <<, QMessageLogger
  Python  print, sys.stdout/sys.stderr .write/.writelines, logging/logger
          .debug/.info/.warning/.error/.exception/.critical/.log, os.write(1|2, ...),
          warnings.warn, traceback.print_*
  QML/JS  console.log/info/warn/error/debug/trace, print

A finding is an identifier inside the call's arguments (string contents are
ignored) whose words (camelCase / snake_case split) include a secret word, or
an argument list / "--cookies" (the jar path travels as an extractor argument).
Python is parsed with ast; C++ and QML/JS lexically, with comments removed.

Usage: python3 -I check_secret_logging.py [--root DIR] [--verbose]
Exit 0 with no findings, 1 with findings, 2 when the scope is broken.
"""
import argparse
import ast
import glob
import os
import re
import sys

SCOPE = [
    "Core/Account/**/*",
    "Clients/Sailfish/Presentation/AccountController.*",
    "Core/Resolver/Infrastructure/YtDlpRunner.*",
    "Core/Worker/**/*",
    "Shared/SailTune/Core/Accounts/**/*",
]
EXTENSIONS = {".cpp", ".cc", ".c", ".h", ".hpp", ".py", ".qml", ".js"}

SECRET_WORDS = {
    "cookie", "cookies", "jar", "jars", "token", "tokens", "sapisid", "apisid", "sapisidhash",
    "authorization", "secret", "secrets", "password", "passwd", "credential", "credentials",
    "sid", "sidcc", "psid", "hsid", "ssid", "bearer", "blob",
}
ARGUMENT_WORDS = {"arguments", "args", "argv", "cmdline", "commandline"}
# Words that only look like a secret word in a compound and are known harmless.
SAFE_IDENTIFIERS = {"qPrintable", "qUtf8Printable"}

CPP_LOGGERS = re.compile(
    r"\b(?:qDebug|qInfo|qWarning|qCritical|qFatal|qCDebug|qCInfo|qCWarning|qCCritical|QMessageLogger"
    r"|printf|fprintf|fputs|puts|perror|syslog|vfprintf|vprintf)\s*\("
    r"|std::(?:cout|cerr|clog)\s*<<|QTextStream\s*\(\s*std(?:out|err)\s*\)\s*<<")
QML_LOGGERS = re.compile(r"\bconsole\s*\.\s*(?:log|info|warn|error|debug|trace)\s*\(|(?<![\w.])print\s*\(")

PY_LOG_METHODS = {"debug", "info", "warning", "warn", "error", "exception", "critical", "log", "fatal"}


def words(identifier):
    parts = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", identifier)
    parts = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", parts)
    return [w.lower() for w in re.split(r"[\s_\d]+", parts) if w]


def classify(identifier):
    if identifier in SAFE_IDENTIFIERS:
        return None
    ws = words(identifier)
    if any(w in SECRET_WORDS for w in ws):
        return "secret-like variable"
    if identifier.lower() in ARGUMENT_WORDS or any(w in ARGUMENT_WORDS for w in ws):
        return "argument list (may carry --cookies <jar>)"
    return None


def strip_c_comments(text):
    """Comments become spaces (newlines kept); string and char literals stay."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def blank_strings(text):
    """String/char literal contents become spaces; returns (code, list of literal contents)."""
    out, literals, i, n = [], [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'`":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            literals.append(text[i + 1:j])
            out.append(c + " " * (j - i - 1) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out), literals


def statement_end(code, start, opening):
    """End of the statement that begins at `start`: the matching ')' of a call,
    extended to ';' for stream-style loggers (`qWarning() << x;`)."""
    depth, i, n = 0, start, len(code)
    if opening:
        while i < n:
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
    j = i + 1
    while j < n and code[j] in " \t\r\n":
        j += 1
    if j < n and code[j] in "<.":   # stream continuation or .noquote() << ...
        k = code.find(";", j)
        return n if k < 0 else k
    if not opening:
        k = code.find(";", start)
        return n if k < 0 else k
    return i


def check_lexical(path, rel, loggers, findings, counter):
    text = strip_c_comments(open(path, encoding="utf-8", errors="replace").read())
    code, _ = blank_strings(text)
    for m in loggers.finditer(code):
        counter[0] += 1
        opening = m.group(0).rstrip().endswith("(")
        start = m.end() - 1 if opening else m.end()
        end = statement_end(code, start, opening)
        segment_code = code[m.end():end]
        segment_text = text[m.end():end]
        line = code.count("\n", 0, m.start()) + 1
        reasons = set()
        for ident in re.findall(r"[A-Za-z_]\w*", segment_code):
            why = classify(ident)
            if why:
                reasons.add("%s '%s'" % (why, ident))
        if "--cookies" in segment_text:
            reasons.add("'--cookies' in a logging call")
        for r in sorted(reasons):
            findings.append("%s:%d: logging call formats %s" % (rel, line, r))


class PyLogging(ast.NodeVisitor):
    def __init__(self, rel, findings, counter):
        self.rel, self.findings, self.counter = rel, findings, counter

    @staticmethod
    def dotted(node):
        parts = []
        while isinstance(node, ast.Attribute):
            parts.append(node.attr)
            node = node.value
        if isinstance(node, ast.Name):
            parts.append(node.id)
        return ".".join(reversed(parts))

    def is_logging(self, call):
        name = self.dotted(call.func)
        if name in ("print", "warnings.warn") or name.startswith("traceback.print"):
            return True
        if name in ("sys.stderr.write", "sys.stdout.write", "sys.stderr.writelines", "sys.stdout.writelines"):
            return True
        if name == "os.write" and call.args and isinstance(call.args[0], ast.Constant) and call.args[0].value in (1, 2):
            return True
        head, _, method = name.rpartition(".")
        if method in PY_LOG_METHODS and head and (head == "logging" or "log" in head.lower()):
            return True
        return False

    def visit_Call(self, node):
        if self.is_logging(node):
            self.counter[0] += 1
            reasons = set()
            values = list(node.args) + [k.value for k in node.keywords if k.arg not in ("file", "end", "sep", "flush")]
            for value in values:
                for sub in ast.walk(value):
                    ident = sub.id if isinstance(sub, ast.Name) else sub.attr if isinstance(sub, ast.Attribute) else None
                    if ident:
                        why = classify(ident)
                        if why:
                            reasons.add("%s '%s'" % (why, ident))
                    if isinstance(sub, ast.Constant) and isinstance(sub.value, str) and "--cookies" in sub.value:
                        reasons.add("'--cookies' in a logging call")
            for r in sorted(reasons):
                self.findings.append("%s:%d: logging call formats %s" % (self.rel, node.lineno, r))
        self.generic_visit(node)


def scope_files(root):
    files, broken = [], []
    for pattern in SCOPE:
        matched = [p for p in glob.glob(os.path.join(root, pattern), recursive=True)
                   if os.path.isfile(p) and os.path.splitext(p)[1] in EXTENSIONS]
        if not matched:
            broken.append(pattern)
        files.extend(matched)
    return sorted(set(files)), broken


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    files, broken = scope_files(root)
    if broken:
        for b in broken:
            print("secret logging check: scope pattern matches no file: %s" % b)
        return 2
    findings, counter = [], [0]
    for path in files:
        rel = os.path.relpath(path, root)
        ext = os.path.splitext(path)[1]
        if args.verbose:
            print("  scan %s" % rel)
        if ext == ".py":
            try:
                tree = ast.parse(open(path, encoding="utf-8").read(), filename=rel)
            except SyntaxError as e:
                findings.append("%s:%s: unparsable Python (%s)" % (rel, e.lineno, e.msg))
                continue
            PyLogging(rel, findings, counter).visit(tree)
        elif ext in (".qml", ".js"):
            check_lexical(path, rel, QML_LOGGERS, findings, counter)
        else:
            check_lexical(path, rel, CPP_LOGGERS, findings, counter)
    for f in findings:
        print(f)
    print("secret logging check: %d files, %d logging calls, %d findings" % (len(files), counter[0], len(findings)))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
