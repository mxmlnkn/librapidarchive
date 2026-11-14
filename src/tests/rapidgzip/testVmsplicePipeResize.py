#!/usr/bin/env python3
"""Regression test for pipe resizing while vmspliced pages are still pending."""

import fcntl
import hashlib
import os
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: testVmsplicePipeResize.py RAPIDGZIP")

    rapidgzip = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        input_path = os.path.join(directory, "input")
        gzip_path = input_path + ".gz"
        with open(input_path, "wb") as input_file:
            input_file.write(os.urandom(4 * 1024 * 1024))
        with open(gzip_path, "wb") as gzip_file:
            subprocess.run(["gzip", "-c", input_path], stdout=gzip_file, check=True)

        with open(input_path, "rb") as input_file:
            expected = hashlib.sha256(input_file.read()).hexdigest()

        process = subprocess.Popen([rapidgzip, "-d", "-c", gzip_path], stdout=subprocess.PIPE)
        assert process.stdout is not None
        read_fd = process.stdout.fileno()

        first = os.read(read_fd, 1 << 20)
        try:
            fcntl.fcntl(read_fd, fcntl.F_SETPIPE_SZ, 1 << 20)
        except OSError:
            pass
        time.sleep(0.2)

        digest = hashlib.sha256(first)
        while True:
            chunk = os.read(read_fd, 1 << 20)
            if not chunk:
                break
            digest.update(chunk)

        returncode = process.wait()
        if returncode != 0 or digest.hexdigest() != expected:
            print(f"vmsplice pipe-resize mismatch: returncode={returncode}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
