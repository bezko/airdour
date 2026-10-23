import re
from pathlib import Path

def resolve(path, mode="union"):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    out = []
    i = 0
    lines = text.splitlines(keepends=True)
    # We'll work without keepends for matching, then rejoin with detected newline
    raw = p.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    lines = text.replace("\r\n", "\n").split("\n")
    # preserve trailing newline
    trailing_nl = text.endswith("\n") or text.endswith("\r\n")

    result = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("<<<<<<<"):
            i += 1
            head = []
            while i < len(lines) and not lines[i].startswith("======="):
                head.append(lines[i]); i += 1
            i += 1  # skip =======
            theirs = []
            while i < len(lines) and not lines[i].startswith(">>>>>>>"):
                theirs.append(lines[i]); i += 1
            i += 1  # skip >>>>>>>
            head_s = "\n".join(head)
            theirs_s = "\n".join(theirs)
            if "smoke_stereo_track" in theirs_s and "smoke_rename" in head_s:
                # special: insert theirs first (with engine_session_ok), then head
                for l in theirs:
                    l2 = l.replace("_engine.snapshot ().session_ok", "engine_session_ok ()")
                    result.append(l2)
                # ensure opener if missing
                block = "\n".join(theirs)
                if "if (ok && smoke_stereo_track" not in block:
                    # insert opener before first content line of theirs
                    inserted = False
                    for l in theirs:
                        if (not inserted) and ("smoke_stereo_track" in l and "if" not in l and "else" not in l):
                            result.insert(len(result)-len(theirs), "\tif (ok && smoke_stereo_track && engine_session_ok ()) {")
                            inserted = True
                        l2 = l.replace("_engine.snapshot ().session_ok", "engine_session_ok ()")
                    # redo cleanly
                    result = result[:-len(theirs)]
                    result.append("\tif (ok && smoke_stereo_track && engine_session_ok ()) {")
                    for l in theirs:
                        result.append(l.replace("_engine.snapshot ().session_ok", "engine_session_ok ()"))
                for l in head:
                    result.append(l)
            elif mode == "union":
                for l in head:
                    result.append(l)
                for l in theirs:
                    result.append(l)
            else:
                raise SystemExit("unknown conflict")
        else:
            result.append(line)
            i += 1

    out_text = newline.join(result)
    if trailing_nl:
        out_text += newline
    # write without adding BOM
    p.write_bytes(out_text.encode("utf-8"))
    remaining = sum(1 for l in result if l.startswith("<<<<<<<") or l.startswith(">>>>>>>"))
    print(f"{path}: resolved, remaining_markers={remaining}")

resolve("win_ardour/engine_host.h")
resolve("win_ardour/engine_host.cc")
resolve("win_ardour/shell_window.cc")
