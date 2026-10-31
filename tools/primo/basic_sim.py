"""A small interpreter for the subset of Primo BASIC that ptest.bas uses,
with a mock CDOS and two drives behind it. Test scaffolding, not an
emulator: it exists so the *logic* of a BASIC program (line targets, error
handling, string arithmetic, file sizes) can be run and fault-injected on the
host before anyone carries it to a real Primo.

What it models, from the Primo manual and the CDOS description:
  * Microsoft-style statements: IF/THEN (a false IF skips the rest of the
    line), FOR/NEXT, GOSUB/RETURN (RETURN discards FOR frames above its
    GOSUB), ON ERROR GOTO / RESUME NEXT, ERR and ERL;
  * relations give -1/0, STR$ has a leading space, PRINT of a number pads
    both sides, strings are limited to 255 characters, variable names are
    significant to two characters;
  * a 16 x 42 text screen that scrolls, saved and compared as a whole;
  * CDOS: CMD SET/CREATE/OPEN/PRINT/PRINT#/INPUT#/GET#/CLOSE/SAVE SCREEN/
    LOAD/TEST/ERROR/CLEAR, EOF(n), the error numbers of chapter 9 (12, 42,
    90, 92, 94, 160, 190+DOS code) and the rule that PRINT#/INPUT#/GET# do
    not poll the drive's error channel.

What it does not model: timing, memory size, real number formatting beyond
integers, anything ptest.bas does not use. Unknown syntax raises SN (ERR 2)
rather than being guessed at.
"""
import math
import re

ERR_SN, ERR_RG, ERR_FC, ERR_UL, ERR_TM, ERR_LS = 2, 4, 8, 14, 24, 28
ERR_NF = 0
ERR_FD, ERR_DEVICE = 42, 160


class RunError(Exception):
    """A BASIC run-time error: what ERR would hold, plus optional extras."""

    def __init__(self, code, text="", count=None):
        super().__init__(f"ERR {code} {text}")
        self.code, self.text, self.count = code, text, count


class Halt(Exception):
    """The program stopped on an error nothing handled (the system prints
    'N XX Error IN line' and returns to the prompt)."""

    def __init__(self, code, line, text="", count=None):
        super().__init__(f"error {code} {text} in line {line}")
        self.code, self.line, self.text, self.count = code, line, text, count


# ---------------------------------------------------------------- screen ----
class Screen:
    ROWS, COLS = 16, 42

    def __init__(self):
        self.scrolls = 0
        self.cls()

    def cls(self):
        self.grid = [[" "] * self.COLS for _ in range(self.ROWS)]
        self.r = self.c = 0

    def newline(self):
        self.c = 0
        if self.r == self.ROWS - 1:
            self.grid = self.grid[1:] + [[" "] * self.COLS]
            self.scrolls += 1
        else:
            self.r += 1

    def put(self, ch):
        if ch in "\r\n":
            return self.newline()
        if self.c >= self.COLS:
            self.newline()
        self.grid[self.r][self.c] = ch
        self.c += 1

    def write(self, text):
        for ch in text:
            self.put(ch)

    def snapshot(self):
        return "".join("".join(row) for row in self.grid)

    def restore(self, snap):
        self.grid = [list(snap[i * self.COLS:(i + 1) * self.COLS]) for i in range(self.ROWS)]

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.grid).rstrip("\n")


# ------------------------------------------------------------------ CDOS ----
class Cdos:
    """Two drives, ten channels, and hooks to break things on purpose."""

    def __init__(self, screen):
        self.screen = screen
        self.drives = {8: {}, 9: {}}        # name -> ("seq", bytes) | ("scr", str)
        self.unit = 8
        self.chan = {}                      # n -> dict
        self.status = {8: (0, "OK", 0, 0), 9: (0, "OK", 0, 0)}
        self.events = []                    # ("close", unit, name, size) ...
        self.tests = 0
        self.failed_writes = 0
        # fault injection -- all default to "behave"
        self.on_close = lambda unit, name, data: data
        self.on_read_line = lambda unit, name, idx, line: line
        self.on_save_screen = lambda snap: snap
        self.on_load_screen = lambda snap: snap
        self.on_test = lambda n_diff, call: n_diff
        self.fail_write_after = None        # bytes written through PRINT# on one channel
        self.create_error = None            # DOS code raised by CMD CREATE (auto-polled)
        self.close_silent_status = None     # (code, text) left in the error channel only
        self.absent = set()                 # units that do not answer

    # -- helpers
    def _check_unit(self):
        if self.unit in self.absent:
            raise RunError(ERR_DEVICE, "DEVICE NOT PRESENT")

    def _drive(self):
        self._check_unit()
        return self.drives.setdefault(self.unit, {})

    def _ch(self, n, mode):
        c = self.chan.get(n)
        if c is None:
            raise RunError(94, "NO")
        if c["mode"] != mode:
            raise RunError(94, "BO")
        if c["unit"] in self.absent:
            raise RunError(ERR_DEVICE, "DEVICE NOT PRESENT")
        return c

    def _open(self, n, name, mode):
        if not 2 <= n <= 14:
            raise RunError(ERR_FC, "FC")
        if n in self.chan:
            raise RunError(92, "FQ")
        if len(self.chan) >= 10:
            raise RunError(90, "BO")
        self.chan[n] = {"unit": self.unit, "name": name, "mode": mode, "buf": bytearray(), "pos": 0, "sent": 0}
        return self.chan[n]

    # -- commands
    def set(self, n):
        self.unit = n

    def command(self, text):
        """CMD PRINT "..." -- the drive's command channel, never polled."""
        self._check_unit()
        m = re.match(r"S0?:(.*)", text)
        if m:
            gone = self.drives.setdefault(self.unit, {}).pop(m.group(1), None)
            self.events.append(("scratch", self.unit, m.group(1), gone is not None))
            self.status[self.unit] = (1, "FILES SCRATCHED", 1 if gone else 0, 0)

    def create(self, n, name):
        drive = self._drive()
        if self.create_error:
            raise RunError(190 + self.create_error, "DOS")
        if n in self.chan:
            raise RunError(92, "FQ")
        if name in drive:
            raise RunError(190 + 63, "FILE EXISTS")
        self._open(n, name, "w")

    def open(self, n, name):
        drive = self._drive()
        if name not in drive:
            raise RunError(190 + 62, "FILE NOT FOUND")
        kind, data = drive[name]
        c = self._open(n, name, "r")
        c["buf"] = bytes(data) if kind == "seq" else b""

    def print_ch(self, n, data):
        c = self._ch(n, "w")
        c["sent"] += len(data)
        if self.fail_write_after is not None and c["sent"] > self.fail_write_after:
            self.failed_writes += 1
            raise RunError(ERR_DEVICE, "DEVICE NOT PRESENT")   # PRINT# does not poll: the drive just stops answering
        c["buf"] += data

    def close(self, n):
        c = self.chan.get(n)
        if c is None:
            raise RunError(94, "NO")
        if c["unit"] in self.absent:
            del self.chan[n]
            raise RunError(ERR_DEVICE, "DEVICE NOT PRESENT")
        if c["mode"] == "w":
            data = self.on_close(c["unit"], c["name"], bytes(c["buf"]))
            self.drives.setdefault(c["unit"], {})[c["name"]] = ("seq", data)
            self.events.append(("close", c["unit"], c["name"], len(data)))
            if self.close_silent_status:
                self.status[c["unit"]] = (self.close_silent_status[0], self.close_silent_status[1], 0, 0)
        del self.chan[n]

    def input_ch(self, n):
        c = self._ch(n, "r")
        buf = c["buf"]
        if c["pos"] >= len(buf):
            raise RunError(ERR_FD, "FD")
        end = buf.find(b"\r", c["pos"])
        end = len(buf) if end < 0 else end
        line = buf[c["pos"]:end].decode("latin-1")
        c["idx"] = c.get("idx", 0) + 1
        c["pos"] = min(end + 1, len(buf))
        return self.on_read_line(c["unit"], c["name"], c["idx"], line)

    def get_ch(self, n):
        c = self._ch(n, "r")
        if c["pos"] >= len(c["buf"]):
            raise RunError(ERR_FD, "FD")
        b = c["buf"][c["pos"]]
        c["pos"] += 1
        return chr(b)

    def eof(self, n):
        c = self._ch(n, "r")
        return -1 if c["pos"] >= len(c["buf"]) else 0

    def save_screen(self, name):
        drive = self._drive()
        if name in drive:
            raise RunError(190 + 63, "FILE EXISTS")
        drive[name] = ("scr", self.on_save_screen(self.screen.snapshot()))

    def load(self, name):
        drive = self._drive()
        if name not in drive:
            raise RunError(190 + 62, "FILE NOT FOUND")
        kind, data = drive[name]
        if kind != "scr":
            raise RunError(199, "NOT A SCREEN FILE (mock)")
        self.screen.restore(self.on_load_screen(data))

    def test(self, name):
        drive = self._drive()
        self.tests += 1
        if name not in drive or drive[name][0] != "scr":
            raise RunError(ERR_FD, "FD", count=None)
        cur = self.screen.snapshot()
        diff = sum(1 for a, b in zip(cur, drive[name][1]) if a != b)
        diff = self.on_test(diff, self.tests)
        if diff:
            raise RunError(ERR_FD, "FD", count=min(diff, 255))

    def error_channel(self):
        self._check_unit()
        st = self.status[self.unit]
        self.status[self.unit] = (0, "OK", 0, 0)
        return st

    def clear(self):
        for n in [n for n, c in self.chan.items() if c["unit"] == self.unit]:
            self.close(n)


# ------------------------------------------------------------ expressions ----
_TOK = re.compile(r"""\s*(?:
     (?P<num>\d+(?:\.\d*)?|\.\d+)
    |(?P<str>"[^"]*"?)
    |(?P<id>[A-Za-z][A-Za-z0-9]*[$%!\#]?)
    |(?P<op><>|<=|>=|=<|=>|[-+*/^<>=(),;\#])
)""", re.X)


def tokenize_expr(text):
    out, pos, text = [], 0, text.rstrip()
    while pos < len(text):
        m = _TOK.match(text, pos)
        if not m or m.end() == pos:
            raise RunError(ERR_SN, f"SN at {text[pos:]!r}")
        pos = m.end()
        kind = m.lastgroup
        val = m.group(kind)
        if kind == "num":
            val = float(val)
        elif kind == "str":
            val = val[1:-1] if val.endswith('"') and len(val) > 1 else val[1:]
        elif kind == "id":
            val = val.upper()
        out.append((kind, val))
    return out


def num_to_str(v):
    if v == int(v):
        s = str(int(v))
    else:
        s = ("%.6g" % v)
    return (" " if v >= 0 else "") + s


def var_key(name):
    """Two significant characters plus the type suffix."""
    suffix = name[-1] if name[-1] in "$%!#" else ""
    core = name[:-1] if suffix else name
    return core[:2] + suffix


def val_of(s):
    m = re.match(r"\s*[-+]?(\d+\.?\d*|\.\d+)", s)
    return float(m.group(0)) if m else 0.0


class Parser:
    NOARG = {"INKEY$", "ERR", "ERL"}

    def __init__(self, sim, text):
        self.sim, self.toks, self.i = sim, tokenize_expr(text), 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def accept(self, kind, val):
        if self.peek() == (kind, val):
            self.i += 1
            return True
        return False

    def expect(self, kind, val):
        if not self.accept(kind, val):
            raise RunError(ERR_SN, f"expected {val!r}, got {self.peek()!r}")

    def done(self):
        return self.i >= len(self.toks)

    # precedence, lowest first
    def expr(self):
        v = self.and_()
        while self.peek() == ("id", "OR"):
            self.take()
            v = float(int(v) | int(self.and_()))
        return v

    def and_(self):
        v = self.not_()
        while self.peek() == ("id", "AND"):
            self.take()
            v = float(int(v) & int(self.not_()))
        return v

    def not_(self):
        if self.peek() == ("id", "NOT"):
            self.take()
            return float(~int(self.not_()))
        return self.rel()

    def rel(self):
        a = self.add()
        kind, op = self.peek()
        if kind == "op" and op in ("=", "<>", "<", ">", "<=", ">=", "=<", "=>"):
            self.take()
            b = self.add()
            if isinstance(a, str) != isinstance(b, str):
                raise RunError(ERR_TM, "TM")
            op = {"=<": "<=", "=>": ">="}.get(op, op)
            r = {"=": a == b, "<>": a != b, "<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
            return -1.0 if r else 0.0
        return a

    def add(self):
        v = self.mul()
        while self.peek() in (("op", "+"), ("op", "-")):
            op = self.take()[1]
            b = self.mul()
            if isinstance(v, str) or isinstance(b, str):
                if op == "-" or not (isinstance(v, str) and isinstance(b, str)):
                    raise RunError(ERR_TM, "TM")
                v = v + b
                if len(v) > 255:
                    raise RunError(ERR_LS, "LS")
            else:
                v = v + b if op == "+" else v - b
        return v

    def mul(self):
        v = self.unary()
        while self.peek() in (("op", "*"), ("op", "/")):
            op = self.take()[1]
            b = self.unary()
            if isinstance(v, str) or isinstance(b, str):
                raise RunError(ERR_TM, "TM")
            if op == "/" and b == 0:
                raise RunError(20, "/0")
            v = v * b if op == "*" else v / b
        return v

    def unary(self):
        if self.accept("op", "-"):
            v = self.unary()
            if isinstance(v, str):
                raise RunError(ERR_TM, "TM")
            return -v
        if self.accept("op", "+"):
            return self.unary()
        return self.pow()

    def pow(self):
        v = self.primary()
        while self.accept("op", "^"):
            v = v ** self.primary()
        return v

    def args(self):
        self.expect("op", "(")
        out = [self.expr()]
        while self.accept("op", ","):
            out.append(self.expr())
        self.expect("op", ")")
        return out

    def primary(self):
        kind, val = self.take()
        if kind == "num" or kind == "str":
            return val
        if kind == "op" and val == "(":
            v = self.expr()
            self.expect("op", ")")
            return v
        if kind != "id":
            raise RunError(ERR_SN, f"unexpected {val!r}")
        sim = self.sim
        if val in self.NOARG:
            return sim.builtin(val, [])
        if val in FUNCS:
            return sim.builtin(val, self.args())
        if self.peek() == ("op", "("):
            return sim.get_array(val, [int(a) for a in self.args()])
        return sim.get_var(val)


FUNCS = {"INT", "LEN", "VAL", "ASC", "CHR$", "STR$", "MID$", "LEFT$", "RIGHT$", "EOF", "FRE", "ABS", "SGN"}


def split_statements(text):
    out, cur, inq = [], [], False
    for ch in text:
        if ch == '"':
            inq = not inq
        if ch == ":" and not inq:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur).strip())
    return [s for s in out if s != ""]


def find_kw(text, kw):
    """Index of the keyword kw outside string literals, or -1."""
    inq = False
    for i, ch in enumerate(text):
        if ch == '"':
            inq = not inq
        elif not inq and text.startswith(kw, i) and (i == 0 or not text[i - 1].isalnum()) \
                and not text[i + len(kw):i + len(kw) + 1].isalnum():
            return i
    return -1


# ---------------------------------------------------------------- program ----
class Jump:
    def __init__(self, pos):
        self.pos = pos


SKIP_LINE = object()
STOP = object()


class Sim:
    def __init__(self, lines, keys=None, max_steps=3_000_000):
        """lines: [(number, text)]; keys(sim) -> str is called for each INKEY$."""
        self.screen = Screen()
        self.cdos = Cdos(self.screen)
        self.lines = [(n, split_statements(t)) for n, t in lines]
        self.index = {n: i for i, (n, _) in enumerate(self.lines)}
        self.keys = keys or (lambda sim: "")
        self.vars, self.arrays = {}, {}
        self.stack = []                 # ("G", pos) | ("F", key, limit, step, pos)
        self.handler = None
        self.errors_seen = []           # (code, line) of every error a handler took
        self.handling = None            # resume position while inside the handler
        self.err = self.erl = 0
        self.steps, self.max_steps = 0, max_steps
        self.max_strings = 0
        self.inkeys = 0
        self.cur_line = 0

    # -- variables
    def get_var(self, name):
        k = var_key(name)
        return self.vars.get(k, "" if k.endswith("$") else 0.0)

    def set_var(self, name, value):
        k = var_key(name)
        if k.endswith("$") != isinstance(value, str):
            raise RunError(ERR_TM, "TM")
        if isinstance(value, str) and len(value) > 255:
            raise RunError(ERR_LS, "LS")
        self.vars[k] = value
        self._track()

    def get_array(self, name, idx):
        k = var_key(name)
        arr = self.arrays.get(k)
        if arr is None or len(idx) != 1 or not 0 <= idx[0] < len(arr):
            raise RunError(9, "BS")
        return arr[idx[0]]

    def set_array(self, name, idx, value):
        k = var_key(name)
        arr = self.arrays.get(k)
        if arr is None or not 0 <= idx < len(arr):
            raise RunError(9, "BS")
        if k.endswith("$") != isinstance(value, str):
            raise RunError(ERR_TM, "TM")
        arr[idx] = value
        self._track()

    def _track(self):
        total = sum(len(v) for v in self.vars.values() if isinstance(v, str))
        total += sum(len(x) for a in self.arrays.values() for x in a if isinstance(x, str))
        self.max_strings = max(self.max_strings, total)

    # -- built-in functions
    def builtin(self, name, a):
        try:
            if name == "INKEY$":
                self.inkeys += 1
                return self.keys(self)
            if name == "ERR":
                return float(self.err)
            if name == "ERL":
                return float(self.erl)
            if name == "INT":
                return float(math.floor(a[0]))
            if name == "ABS":
                return abs(a[0])
            if name == "SGN":
                return float((a[0] > 0) - (a[0] < 0))
            if name == "FRE":
                return 5000.0
            if name == "LEN":
                return float(len(a[0]))
            if name == "VAL":
                return val_of(a[0])
            if name == "ASC":
                if not a[0]:
                    raise RunError(ERR_FC, "FC")
                return float(ord(a[0][0]))
            if name == "CHR$":
                if not 0 <= a[0] <= 255:
                    raise RunError(ERR_FC, "FC")
                return chr(int(a[0]))
            if name == "STR$":
                return num_to_str(a[0])
            if name == "EOF":
                return float(self.cdos.eof(int(a[0])))
            if name in ("LEFT$", "RIGHT$"):
                s, n = a[0], int(a[1])
                if n < 0:
                    raise RunError(ERR_FC, "FC")
                return s[:n] if name == "LEFT$" else (s[-n:] if n else "")
            if name == "MID$":
                s, i = a[0], int(a[1])
                if i < 1:
                    raise RunError(ERR_FC, "FC")
                n = int(a[2]) if len(a) > 2 else len(s)
                return s[i - 1:i - 1 + n]
        except TypeError:
            raise RunError(ERR_TM, "TM")
        raise RunError(ERR_SN, f"unknown function {name}")

    # -- running
    def run(self):
        pos = (0, 0)
        while True:
            li, si = pos
            if li >= len(self.lines):
                return
            num, stmts = self.lines[li]
            if si >= len(stmts):
                pos = (li + 1, 0)
                continue
            self.steps += 1
            if self.steps > self.max_steps:
                raise Halt(-1, num, "step limit")
            self.cur_line = num
            try:
                r = self.exec(stmts[si], (li, si))
            except RunError as e:
                pos = self.on_error(e, num, (li, si))
                continue
            if r is None:
                pos = (li, si + 1)
            elif r is SKIP_LINE:
                pos = (li + 1, 0)
            elif r is STOP:
                return
            else:
                pos = r.pos

    def on_error(self, e, line, at):
        if self.handler is None or self.handling is not None:      # unhandled, or an error inside the handler
            raise Halt(e.code, line, e.text, e.count)
        self.errors_seen.append((e.code, line))
        self.err, self.erl = e.code, line
        self.handling = (at[0], at[1] + 1)
        return self.line_pos(self.handler)

    def line_pos(self, n):
        if n not in self.index:
            raise RunError(ERR_UL, f"UL {n}")
        return (self.index[n], 0)

    # -- statements
    def exec(self, text, at):
        m = re.match(r"(IF|FOR|NEXT|GOTO|GOSUB|RETURN|END|CLS|PRINT|CLEAR|DIM|REM|LET|RESUME|CMD|ON)\b\s*(.*)$", text, re.S)
        if not m:
            return self.assign(text)
        kw, rest = m.groups()
        if kw == "REM":
            return None
        if kw == "LET":
            return self.assign(rest)
        if kw == "IF":
            return self.do_if(rest, at)
        if kw == "FOR":
            return self.do_for(rest, at)
        if kw == "NEXT":
            return self.do_next(rest)
        if kw == "GOTO":
            return Jump(self.line_pos(int(rest)))
        if kw == "GOSUB":
            self.stack.append(("G", (at[0], at[1] + 1)))
            return Jump(self.line_pos(int(rest)))
        if kw == "RETURN":
            while self.stack:
                fr = self.stack.pop()
                if fr[0] == "G":
                    return Jump(fr[1])
            raise RunError(ERR_RG, "RG")
        if kw == "END":
            return STOP
        if kw == "CLS":
            self.screen.cls()
            return None
        if kw == "CLEAR":
            self.vars, self.arrays, self.stack = {}, {}, []
            self.cdos.chan.clear()
            return None
        if kw == "DIM":
            mm = re.match(r"([A-Za-z][A-Za-z0-9]*[$%!#]?)\((\d+)\)$", rest.strip())
            if not mm:
                raise RunError(ERR_SN, "DIM")
            k = var_key(mm.group(1).upper())
            self.arrays[k] = [("" if k.endswith("$") else 0.0)] * (int(mm.group(2)) + 1)
            return None
        if kw == "PRINT":
            self.do_print(rest, self.screen.write)
            return None
        if kw == "RESUME":
            if rest.strip().upper() != "NEXT":
                raise RunError(ERR_SN, "RESUME")
            if self.handling is None:
                raise RunError(36, "RW")
            pos, self.handling = self.handling, None
            return Jump(pos)
        if kw == "ON":
            mm = re.match(r"ERROR\s+GOTO\s+(\d+)$", rest.strip())
            if not mm:
                raise RunError(ERR_SN, "ON")
            n = int(mm.group(1))
            self.handler = n or None
            return None
        if kw == "CMD":
            return self.do_cmd(rest)
        raise RunError(ERR_SN, kw)

    def assign(self, text):
        p = Parser(self, text)
        kind, name = p.take()
        if kind != "id":
            raise RunError(ERR_SN, text)
        idx = None
        if p.accept("op", "("):
            idx = int(p.expr())
            p.expect("op", ")")
        p.expect("op", "=")
        v = p.expr()
        if not p.done():
            raise RunError(ERR_SN, text)
        if idx is None:
            self.set_var(name, v)
        else:
            self.set_array(name, idx, v)
        return None

    def cond(self, text):
        p = Parser(self, text)
        v = p.expr()
        if not p.done():
            raise RunError(ERR_SN, text)
        if isinstance(v, str):
            raise RunError(ERR_TM, "TM")
        return v != 0

    def do_if(self, rest, at):
        k = find_kw(rest, "THEN")
        if k < 0:
            raise RunError(ERR_SN, "IF without THEN")
        test, body = rest[:k], rest[k + 4:].strip()
        if not self.cond(test):
            return SKIP_LINE
        if body.isdigit():
            return Jump(self.line_pos(int(body)))
        return self.exec(body, at)

    def do_for(self, rest, at):
        k = find_kw(rest, "TO")
        if k < 0:
            raise RunError(ERR_SN, "FOR")
        head, tail = rest[:k], rest[k + 2:]
        s = find_kw(tail, "STEP")
        limit_text, step_text = (tail, "1") if s < 0 else (tail[:s], tail[s + 4:])
        name, _, start = head.partition("=")
        key = var_key(name.strip().upper())
        self.assign(f"{name.strip()}={start}")
        limit, step = Parser(self, limit_text).expr(), Parser(self, step_text).expr()
        # a FOR on a variable already in use drops its old frame and everything above it
        for i, fr in enumerate(self.stack):
            if fr[0] == "F" and fr[1] == key:
                del self.stack[i:]
                break
        first = self.get_var(name.strip().upper())
        if (step >= 0 and first > limit) or (step < 0 and first < limit):
            raise AssertionError(f"line {self.cur_line}: empty FOR loop -- BASICs disagree on what happens, do not write one")
        self.stack.append(("F", key, limit, step, (at[0], at[1] + 1)))
        return None

    def do_next(self, rest):
        key = var_key(rest.strip().upper()) if rest.strip() else None
        while self.stack:
            fr = self.stack[-1]
            if fr[0] == "G":
                break
            if key is None or fr[1] == key:
                _, k, limit, step, pos = fr
                v = self.vars.get(k, 0.0) + step
                self.vars[k] = v
                if (step >= 0 and v <= limit) or (step < 0 and v >= limit):
                    return Jump(pos)
                self.stack.pop()
                return None
            self.stack.pop()
        raise RunError(ERR_NF, "NF")

    def do_print(self, rest, emit):
        p = Parser(self, rest)
        newline = True
        while not p.done():
            if p.accept("op", ";"):
                newline = False
                continue
            if p.accept("op", ","):
                newline = False
                emit(" ")
                continue
            newline = True
            v = p.expr()
            emit(v if isinstance(v, str) else num_to_str(v) + " ")
        if newline:
            emit("\r")

    # -- CDOS statements
    def do_cmd(self, rest):
        cd = self.cdos
        m = re.match(r"(SET|PRINT|CREATE|OPEN|INPUT|GET|CLOSE|SAVE\s+SCREEN|LOAD|TEST|ERROR|CLEAR)\b\s*(.*)$", rest, re.S)
        if not m:
            raise RunError(ERR_SN, f"CMD {rest}")
        sub, arg = re.sub(r"\s+", " ", m.group(1)), m.group(2)
        P = lambda t: Parser(self, t)
        if sub == "SET":
            cd.set(int(P(arg).expr()))
        elif sub == "PRINT" and arg.startswith("#"):
            p = P(arg[1:])
            ch = int(p.expr())
            p.expect("op", ",")
            buf = []
            newline = True
            while not p.done():
                if p.accept("op", ";"):
                    newline = False
                    continue
                if p.accept("op", ","):
                    buf.append(",")
                    newline = False
                    continue
                newline = True
                v = p.expr()
                buf.append(v if isinstance(v, str) else num_to_str(v) + " ")
            if newline:
                buf.append("\r")
            cd.print_ch(ch, "".join(buf).encode("latin-1"))
        elif sub == "PRINT":
            out = []
            self.do_print(arg, out.append)
            cd.command("".join(out).rstrip("\r"))
        elif sub in ("CREATE", "OPEN"):
            p = P(arg)
            ch = int(p.expr())
            p.expect("op", ",")
            name = p.expr()
            (cd.create if sub == "CREATE" else cd.open)(ch, name)
        elif sub == "INPUT":
            p = P(arg.lstrip("#"))
            ch = int(p.expr())
            p.expect("op", ",")
            line = cd.input_ch(ch)
            self.set_var(p.take()[1], line)
        elif sub == "GET":
            p = P(arg.lstrip("#"))
            ch = int(p.expr())
            names = []
            while p.accept("op", ","):
                names.append(p.take()[1])
            for nm in names:
                self.set_var(nm, cd.get_ch(ch))
        elif sub == "CLOSE":
            cd.close(int(P(arg).expr()))
        elif sub == "SAVE SCREEN":
            cd.save_screen(P(arg).expr())
        elif sub == "LOAD":
            cd.load(P(arg).expr())
        elif sub == "TEST":
            cd.test(P(arg).expr())
        elif sub == "ERROR":
            names = [t.strip().upper() for t in arg.split(",")]
            code, text, trk, sec = cd.error_channel()
            for nm, val in zip(names, (float(code), text, float(trk), float(sec))):
                self.set_var(nm, val)
        elif sub == "CLEAR":
            cd.clear()
        return None


def load_source(text):
    """Source text -> [(number, text)] via the same parser the tokenizer uses."""
    import primo_basic
    return primo_basic.parse_source(text)


def load_prg(prg):
    """A tokenized file -> [(number, text)], through the detokenizer: what runs
    is what the Primo would see."""
    import primo_basic
    payload = primo_basic.prg_payload(prg)
    return [(n, primo_basic.to_source(t)) for n, t in primo_basic.payload_lines(payload)]
