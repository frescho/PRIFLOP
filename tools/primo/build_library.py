#!/usr/bin/env python3
"""Convert the fetched Primo tape library into a zip of drive files.

  ./build_library.py [<library dir>] <out.zip>

<library dir> is what fetch-library.sh made (default: primo-library/ at the
top of the repository). Every ptp/<folder>/<name>.ptp becomes
<FOLDER>/<NAME>.PRG in the zip, converted by ptp2disk.py with --salvage
(the 13 files whose checksums follow another rule; ptp2disk.py says which).
A part that only loads the next one from tape is left out (ptp2disk.py,
is_tape_autoloader).
The .prg files already in disk format (ptp/cdos/) are copied unchanged.
BASIC programs get CMD SAVE AUTO's record, so CMD LOAD runs them as it
would a program saved with AUTO. A .ptp holding several programs (a
loader and its parts, a series) gives NAME, then NAME-2, NAME-3 ... in
tape order; a tape loader still asks the tape for its parts, so those
programs only work where the parts stand alone.
Programs written for the 32 KB Primo (A32_PROGRAMS) get a32fix's patch, so
they run on the 64 KB one.
bin/ is left out: it holds CDOS's own boot files, which are not ours to
ship.

Names are upper case and cut to 16 characters, the most a CBM file name
holds, so `CMD LOAD"NAME"` reaches every program. A cut that would make
two names in one folder equal stops the build.

The programs belong to their authors; the zip is for the user's own use
(see fetch-library.sh). Upload it with the web page, folder by folder.
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a32fix  # noqa: E402
import ptp2disk  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
NAME_MAX = 16
# "(A32)" in the library's notes, and failing on the 64 KB Primo with OV/FC
# until patched (2026-10-15). Folder/file as in ptp/.
A32_PROGRAMS = {"jatek/banyasz_berci.ptp", "jatek/lokomotiv.ptp"}


def drive_name(stem):
    return stem.upper()[:NAME_MAX] + ".PRG"


def build(library, out=None):
    """{zip path: bytes}, [warnings]. Writes the zip when `out` is given."""
    files, warnings = {}, []
    ptp_root = os.path.join(library, "ptp")
    for folder in sorted(os.listdir(ptp_root)):
        src_dir = os.path.join(ptp_root, folder)
        if not os.path.isdir(src_dir):
            continue
        for fn in sorted(os.listdir(src_dir)):
            stem, ext = os.path.splitext(fn)
            data = open(os.path.join(src_dir, fn), "rb").read()
            if ext.lower() == ".ptp":
                w = []
                stem_name = lambda n: [stem.upper()[:NAME_MAX] if i == 0
                                       else f"{stem.upper()[:NAME_MAX - 3]}-{i + 1}" for i in range(n)]
                progs = [d for _, d in ptp2disk.convert_all(data, salvage=True, warnings=w, autorun=True,
                                                            names=stem_name,
                                                            basic_fix=a32fix.fix_payload
                                                            if f"{folder}/{fn}" in A32_PROGRAMS else None)]
                warnings += [f"{folder}/{fn}: {x}" for x in w]
            elif ext.lower() == ".prg":
                progs = [data]
            else:
                warnings.append(f"{folder}/{fn}: skipped, not .ptp or .prg")
                continue
            for i, d in enumerate(progs):
                name = drive_name(stem) if i == 0 else drive_name(f"{stem.upper()[:NAME_MAX - 3]}-{i + 1}")
                path = f"{folder.upper()}/{name}"
                if path in files:
                    raise ValueError(f"{folder}/{fn}: {path} already taken -- two names cut to the same 16")
                files[path] = d
    if out:
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for path in sorted(files):
                z.writestr(path, files[path])
    return files, warnings


def main():
    args = sys.argv[1:]
    if len(args) not in (1, 2):
        print(__doc__)
        sys.exit(1)
    library = args[0] if len(args) == 2 else os.path.join(ROOT, "primo-library")
    out = args[-1]
    files, warnings = build(library, out)
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    print(f"{len(files)} files, {sum(map(len, files.values()))} bytes unpacked, "
          f"{os.path.getsize(out)} bytes zipped -> {out}")


if __name__ == "__main__":
    main()
