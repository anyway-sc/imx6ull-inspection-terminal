"""Run the SysV wrapper with isolated files and MSYS processes; never contact the board."""
from pathlib import Path
import concurrent.futures
import os
import subprocess
import tempfile
import time


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/vehicle-stack"
SHELL = os.environ.get("SR501_TEST_SHELL", "D:/Git/usr/bin/sh.exe")
CHECKS = 0


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def run(env, *args):
    return subprocess.run(
        [SHELL, str(SCRIPT), *args], env=env, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=12,
    )


def until(predicate, message, seconds=7):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.08)
    status = run(env, "status")
    details = logfile.read_text(errors="replace") if logfile.exists() else "(no log)"
    trace = subprocess.run(
        [SHELL, "-x", str(SCRIPT), "status"], env=env,
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=5,
    )
    details += "\nSTATUS TRACE:\n" + trace.stdout + trace.stderr
    if pidfile.exists():
        details += "\nPID RECORD: " + pidfile.read_text()
        fixture_pid = pidfile.read_text().split()[0]
        probe = subprocess.run(
            [SHELL, "-c", 'readlink "/proc/$1/exe"; readlink -f /usr/bin/sleep; cat "/proc/$1/stat"; tr "\\000" "\\n" < "/proc/$1/cmdline"', "probe", fixture_pid],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=5,
        )
        details += "\n" + probe.stdout + probe.stderr
    raise AssertionError(message + "\n" + status.stdout + status.stderr + details)


with tempfile.TemporaryDirectory(prefix="sysv-fixture-", dir=Path.cwd()) as temporary:
    base = Path(temporary)
    sysroot = base / "sys"
    pidfile = base / "run/vehicle-stack.pid"
    logfile = base / "vehicle.log"
    events = base / "events"
    usb = base / "usb.sh"
    entry = base / "server.sh"
    mounts = base / "mounts"
    commands = base / "bin"
    # Linux preserves /proc starttime across exec. MSYS restarts the Windows
    # process during exec, so normalize only this field in the local fixture.
    # The actual PID, executable, command line, and signals remain real.
    write(commands / "awk", """#!/bin/sh
case ${2:-} in
    /proc/*/stat) exec /usr/bin/awk '$3 == "Z" { exit 1 } { print 42 }' "$2" ;;
    *) exec /usr/bin/awk "$@" ;;
esac
""")
    write(sysroot / "kernel/debug/pinctrl/20e0000.iomuxc/pinmux-pins", "fixture\n")
    write(sysroot / "class/misc/ap3216c/als", "218\n")
    write(mounts, f"none {sysroot.as_posix()}/kernel/debug debugfs rw 0 0\n")
    write(usb, """#!/bin/sh
printf 'USB_BEGIN\\n' >> "$VEHICLE_STACK_FIXTURE_EVENTS"
sleep "${VEHICLE_STACK_FIXTURE_USB_DELAY:-0}"
if [ "${VEHICLE_STACK_FIXTURE_USB_FAIL:-0}" = 1 ]; then
    echo 'fixture: USB failure'
    exit 1
fi
printf 'USB_READY\\n' >> "$VEHICLE_STACK_FIXTURE_EVENTS"
""")
    write(entry, """#!/bin/sh
printf 'SERVER_ENTRY\\n' >> "$VEHICLE_STACK_FIXTURE_EVENTS"
sleep "${VEHICLE_STACK_FIXTURE_ENTRY_DELAY:-0}"
printf 'SERVER_RUNNING\\n' >> "$VEHICLE_STACK_FIXTURE_EVENTS"
exec /usr/bin/sleep 120
""")
    env = os.environ.copy()
    commands_path = "/" + commands.drive[0].lower() + commands.as_posix()[2:]
    env.update({
        "PATH": commands_path + ":/d/Git/usr/bin:/d/Git/bin",
        "VEHICLE_STACK_RUN_DIR": (base / "run").as_posix(),
        "VEHICLE_STACK_LOG": logfile.as_posix(),
        "VEHICLE_STACK_USB_START": usb.as_posix(),
        "VEHICLE_STACK_SERVER_START": entry.as_posix(),
        "VEHICLE_STACK_CAMERA": "/dev/null",
        "VEHICLE_STACK_MOUNTS": mounts.as_posix(),
        "VEHICLE_STACK_WAIT_SECONDS": "1",
        "VEHICLE_STACK_FIXTURE_EVENTS": events.as_posix(),
        "VEHICLE_SYS_ROOT": sysroot.as_posix(),
        "VEHICLE_SERVER_APP": "/usr/bin/sleep",
    })

    def event_lines():
        return events.read_text().splitlines() if events.exists() else []

    def is_running():
        status = run(env, "status")
        return status.returncode == 0 and ": running (PID" in status.stdout

    def stopped():
        return run(env, "status").returncode == 3

    def reset():
        result = run(env, "stop")
        check(result.returncode == 0, result.stdout + result.stderr)
        if events.exists():
            events.unlink()

    try:
        syntax = subprocess.run([SHELL, "-n", str(SCRIPT)], capture_output=True, timeout=8)
        check(syntax.returncode == 0, syntax.stderr.decode(errors="replace"))
        check(stopped(), "An unstarted stack must report stopped.")
        check(run(env, "bogus").returncode == 2, "Unknown actions must fail.")

        started = run(env, "start")
        check(started.returncode == 0, started.stdout + started.stderr)
        until(is_running, "The stack did not execute the mock server.")
        check(event_lines() == ["USB_BEGIN", "USB_READY", "SERVER_ENTRY", "SERVER_RUNNING"],
              "USB must complete before starting the server.")
        saved_pid = pidfile.read_text()
        again = run(env, "start")
        check(again.returncode == 0 and "already running" in again.stdout,
              "A duplicate start must reuse the existing process.")
        check(pidfile.read_text() == saved_pid and len(event_lines()) == 4,
              "A duplicate start must not rerun USB or replace the PID.")

        # Simulate a reused PID by altering its recorded start time.
        pid, ticks = saved_pid.split()
        write(pidfile, f"{pid} {int(ticks) + 1}\n")
        rejected = run(env, "stop")
        check(rejected.returncode == 0 and "no process was signalled" in rejected.stdout,
              "A stale process record must never authorize signalling.")
        write(pidfile, saved_pid)
        check(is_running(), "The process protected by the start-time check was killed.")

        # A matching PID/time with the wrong executable must also be rejected.
        other_env = env | {"VEHICLE_SERVER_APP": "/usr/bin/cat"}
        rejected = run(other_env, "stop")
        check(rejected.returncode == 0 and "no process was signalled" in rejected.stdout,
              "Executable identity must also match before stopping.")
        write(pidfile, saved_pid)
        check(is_running(), "An executable mismatch must not kill the process.")
        reset()
        check(stopped() and not pidfile.exists(), "Stop must remove the active PID record.")
        check(run(env, "stop").returncode == 0, "Repeated stop must succeed.")

        # The existing server entry can spend time loading modules before exec.
        slow_env = env | {"VEHICLE_STACK_FIXTURE_ENTRY_DELAY": "2"}
        check(run(slow_env, "start").returncode == 0, "Slow entry launch failed.")
        until(lambda: "SERVER_ENTRY" in event_lines(), "The slow entry was not reached.")
        status = run(env, "status")
        check(status.returncode == 0 and "initializing" in status.stdout,
              "The server entry must remain a managed process during module setup.")
        again = run(env, "start")
        check(again.returncode == 0 and "already initializing" in again.stdout,
              "A start during initialization must not create a second worker.")
        until(is_running, "The slow entry did not finish.")
        check(event_lines().count("USB_BEGIN") == 1, "Initialization was duplicated.")
        reset()

        # Failure must be visible in the log without starting the server.
        fail_env = env | {"VEHICLE_STACK_FIXTURE_USB_FAIL": "1"}
        run(fail_env, "start")
        until(stopped, "USB failure left a live stack process.")
        check("SERVER_ENTRY" not in event_lines(), "Server ran after USB failure.")
        check("USB/ADB initialization failed" in logfile.read_text(),
              "USB failure must be recorded in the log.")
        reset()

        missing_env = env | {"VEHICLE_STACK_CAMERA": (base / "missing-camera").as_posix()}
        run(missing_env, "start")
        until(lambda: "USB_READY" in event_lines(), "Camera check was not reached.")
        until(stopped, "Missing hardware did not terminate the worker.")
        check("SERVER_ENTRY" not in event_lines() and "devices not ready after 1s" in logfile.read_text(),
              "Missing hardware must time out and be logged.")
        reset()

        for invalid in ("0", "61", "not-a-number"):
            result = run(env | {"VEHICLE_STACK_WAIT_SECONDS": invalid}, "start")
            check(result.returncode != 0 and not event_lines(),
                  "Invalid wait settings must fail before launching a worker.")

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: run(env, "start"), range(2)))
        check(any(result.returncode == 0 for result in results), "Both concurrent starts failed.")
        until(is_running, "Concurrent start did not produce a running server.")
        check(event_lines().count("USB_BEGIN") == 1, "Concurrent starts launched multiple workers.")
        reset()

        for malformed in ("1 1\n", "not-a-pid\n", "999999999 1\n"):
            write(pidfile, malformed)
            result = run(env, "stop")
            check(result.returncode == 0 and "no process was signalled" in result.stdout,
                  "Malformed, PID 1, or nonexistent records must not authorize a signal.")
    finally:
        # The fixture owns every PID it records; no board or user process is used.
        run(env, "stop")

print(f"PASS: {CHECKS} SysV wrapper checks; isolated files and MSYS processes only.")
