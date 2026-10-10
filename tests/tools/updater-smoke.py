#!/usr/bin/env python3
"""Smoke test of the Windows updater (lib/install/updater_win): the real
Updater.exe swaps a small fake install directory and starts "the app" again.

    python tests/tools/updater-smoke.py --updater Updater.exe \
        --probe-old probe_old.exe --probe-new probe_new.exe [--wine wine]

The probes are tests/tools/updater_probe.cpp built twice (PROBE_NAME "old" and
"new"); CMake builds them on Windows and registers this script with ctest.
With --wine the same test runs on Linux against a cross-compiled updater.

What is checked:

  1. the updater waits for the running app (a probe that stays alive for a few
     seconds), then: new files in, the stale file of the old list out, files
     that are not the app's untouched, no backup / journal / staging left, the
     result file says "installed", and the new probe was started with exactly
     the arguments of the request (spaces, quotes, a trailing backslash,
     Persian text, an empty argument);
  2. a new version without the executable is refused: nothing changes, the
     result says "unchanged", the old probe is started again;
  3. a swap that was cut off (a journal and a half-moved file) is undone before
     the new install runs.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

ARGUMENTS = [
    "--workdir=C:\\Users\\some one\\data dir\\",
    "plain",
    'with "quotes" inside',
    "back\\slash\\\\double",
    "ends with backslash\\",
    "\u0641\u0627\u0631\u0633\u06cc \u0645\u062a\u0646",
    "",
    "--last",
]


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(content, bytes) else "w"
    with open(path, mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": "\n"})) as stream:
        stream.write(content)


def read(path):
    with open(path, "rb") as stream:
        return stream.read()


def escape(value):
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


class Setup:
    def __init__(self, options, root):
        self.options = options
        self.root = root
        self.install = os.path.join(root, "install dir")
        self.updates = os.path.join(root, "data", "updates")
        self.staged = os.path.join(self.updates, "0.2.0")
        self.result = os.path.join(self.updates, "install.result")
        self.log = os.path.join(self.updates, "install.log")
        self.params = os.path.join(self.updates, "install.params")
        self.updater = os.path.join(self.updates, "Updater.exe")
        self.output = os.path.join(self.install, "probe-output.txt")

    def native(self, path):
        """The path as a Windows program sees it."""
        if not self.options.wine:
            return path
        return "Z:" + path.replace("/", "\\")

    def command(self, program, *arguments):
        prefix = [self.options.wine] if self.options.wine else []
        return prefix + [program] + list(arguments)

    def make_install(self):
        os.makedirs(self.install, exist_ok=True)
        shutil.copyfile(self.options.probe_old, os.path.join(self.install, "Eleza.exe"))
        write(os.path.join(self.install, "lib", "core.dll"), "old core")
        write(os.path.join(self.install, "plugins", "old", "stale.dll"), "stale")
        write(os.path.join(self.install, "unins000.dat"), "the uninstaller's")
        write(os.path.join(self.install, "eleza-files.list"),
              "Eleza.exe\neleza-files.list\nlib/core.dll\nplugins/old/stale.dll\n")

    def make_staged(self, with_executable=True):
        shutil.rmtree(self.staged, ignore_errors=True)
        os.makedirs(self.staged)
        if with_executable:
            shutil.copyfile(self.options.probe_new, os.path.join(self.staged, "Eleza.exe"))
        write(os.path.join(self.staged, "lib", "core.dll"), "new core")
        write(os.path.join(self.staged, "plugins", "new", "added.dll"), "added")
        write(os.path.join(self.staged, "eleza-files.list"),
              "Eleza.exe\neleza-files.list\nlib/core.dll\nplugins/new/added.dll\n")

    def write_params(self, pid):
        lines = [
            "pid=%d" % pid,
            "install=" + escape(self.native(self.install)),
            "staged=" + escape(self.native(self.staged)),
            "version=0.2.0",
            "executable=Eleza.exe",
            "result=" + escape(self.native(self.result)),
            "log=" + escape(self.native(self.log)),
        ] + ["argument=" + escape(argument) for argument in ARGUMENTS]
        write(self.params, "\n".join(lines) + "\n")
        os.makedirs(self.updates, exist_ok=True)
        shutil.copyfile(self.options.updater, self.updater)
        for path in (self.result, self.output):
            if os.path.exists(path):
                os.remove(path)

    def run_updater(self, timeout=120):
        started = time.time()
        code = subprocess.call(
            self.command(self.updater, "--params=" + self.native(self.params)),
            timeout=timeout)
        return code, time.time() - started

    def wait_for_probe(self, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(self.output):
                lines = read(self.output).decode("utf-8").split("\n")
                return lines[0], int(lines[1]), lines[2:-1]
            time.sleep(0.2)
        raise Failure("the app was not started again")

    def result_fields(self):
        check(os.path.exists(self.result), "no result file")
        fields = {}
        for line in read(self.result).decode("utf-8").splitlines():
            key, _, value = line.partition("=")
            fields[key] = value
        return fields

    def leftovers(self):
        return sorted(name for name in os.listdir(self.install) if name.startswith("_"))


def scenario_install(setup):
    setup.make_install()
    setup.make_staged()
    # "The running app": alive for a few seconds after the updater starts.
    running = subprocess.Popen(setup.command(
        os.path.join(setup.install, "Eleza.exe"), "--probe-sleep=4000"))
    name, pid, _ = setup.wait_for_probe()
    check(name == "old", "the old probe did not start")
    setup.write_params(pid)
    code, elapsed = setup.run_updater()
    running.wait(timeout=30)
    check(code == 0, "the updater exited with %d\n%s" % (code, log_text(setup)))
    check(elapsed >= 2.0, "the updater did not wait for the app (%.1f s)" % elapsed)

    fields = setup.result_fields()
    check(fields.get("status") == "installed", "result: %r" % fields)
    check(fields.get("version") == "0.2.0", "result: %r" % fields)
    check(read(os.path.join(setup.install, "Eleza.exe")) == read(setup.options.probe_new),
          "Eleza.exe is not the new one")
    check(read(os.path.join(setup.install, "lib", "core.dll")) == b"new core", "core.dll is old")
    check(read(os.path.join(setup.install, "plugins", "new", "added.dll")) == b"added", "added.dll")
    check(not os.path.exists(os.path.join(setup.install, "plugins", "old")),
          "the stale file (or its directory) is still there")
    check(read(os.path.join(setup.install, "unins000.dat")) == b"the uninstaller's",
          "a file that is not the app's was touched")
    name, _, arguments = setup.wait_for_probe()
    check(name == "new", "what was started is not the new version")
    check(arguments == ARGUMENTS, "arguments: %r" % (arguments,))
    check(setup.leftovers() == [], "left behind: %r" % setup.leftovers())
    check(not os.path.exists(setup.staged), "the staging directory was not removed")
    check("installed 0.2.0" in log_text(setup), "the log:\n" + log_text(setup))
    return "installed after waiting %.1f s for the app; relaunched with %d arguments intact" % (
        elapsed, len(arguments))


def scenario_refused(setup):
    shutil.rmtree(setup.install, ignore_errors=True)
    setup.make_install()
    setup.make_staged(with_executable=False)
    before = snapshot(setup.install)
    setup.write_params(0)
    code, _ = setup.run_updater()
    check(code == 1, "the updater exited with %d" % code)
    fields = setup.result_fields()
    check(fields.get("status") == "unchanged", "result: %r" % fields)
    check(fields.get("error"), "no reason in the result")
    name, _, arguments = setup.wait_for_probe()
    check(name == "old", "the old version was not started again")
    check(arguments == ARGUMENTS, "arguments: %r" % (arguments,))
    after = snapshot(setup.install)
    after.pop("probe-output.txt", None)
    check(after == before, "the install directory changed")
    return "refused (%s); the old version was started again" % fields["error"]


def scenario_recover(setup):
    shutil.rmtree(setup.install, ignore_errors=True)
    setup.make_install()
    setup.make_staged()
    # An earlier run died after moving core.dll out: the journal says so.
    backup = os.path.join(setup.install, "_backup_0.1.5", "lib")
    os.makedirs(backup)
    os.replace(os.path.join(setup.install, "lib", "core.dll"), os.path.join(backup, "core.dll"))
    write(os.path.join(setup.install, "_update_0.1.5", "lib", "core.dll"), "never arrived")
    write(os.path.join(setup.install, "_update_journal.txt"),
          "eleza-update-journal 1\nversion 0.1.5\nout lib/core.dll\nin lib/co")
    setup.write_params(0)
    code, _ = setup.run_updater()
    check(code == 0, "the updater exited with %d\n%s" % (code, log_text(setup)))
    text = log_text(setup)
    check("undoing the unfinished install of 0.1.5" in text, "the log:\n" + text)
    check(read(os.path.join(setup.install, "lib", "core.dll")) == b"new core", "core.dll")
    name, _, _ = setup.wait_for_probe()
    check(name == "new", "the new version was not started")
    check(setup.leftovers() == [], "left behind: %r" % setup.leftovers())
    return "the cut-off install of 0.1.5 was undone first, then 0.2.0 was installed"


def snapshot(root):
    result = {}
    for directory, _, files in os.walk(root):
        for name in files:
            path = os.path.join(directory, name)
            result[os.path.relpath(path, root).replace(os.sep, "/")] = read(path)
    return result


def log_text(setup):
    return read(setup.log).decode("utf-8", "replace") if os.path.exists(setup.log) else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--updater", required=True)
    parser.add_argument("--probe-old", required=True)
    parser.add_argument("--probe-new", required=True)
    parser.add_argument("--wine")
    options = parser.parse_args()
    for path in (options.updater, options.probe_old, options.probe_new):
        if not os.path.isfile(path):
            sys.exit("updater-smoke: no such file: " + path)
    if os.name != "nt" and not options.wine:
        sys.exit("updater-smoke: this is a Windows program; pass --wine <wine> on Linux")

    failed = False
    for scenario in (scenario_install, scenario_refused, scenario_recover):
        root = tempfile.mkdtemp(prefix="eleza-updater-smoke-")
        try:
            print("%s: %s" % (scenario.__name__, scenario(Setup(options, root))))
        except Failure as failure:
            failed = True
            print("%s: FAILED: %s" % (scenario.__name__, failure))
        finally:
            shutil.rmtree(root, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
