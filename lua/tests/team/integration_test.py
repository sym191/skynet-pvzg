#!/usr/bin/env python3
"""Exercise original-Skynet service RPCs and the listener-free launcher."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def assert_no_listeners(pid):
    inodes = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = fd.readlink().as_posix()
        except FileNotFoundError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    for table in ("tcp", "tcp6", "udp", "udp6"):
        for row in Path(f"/proc/{pid}/net/{table}").read_text().splitlines()[1:]:
            fields = row.split()
            assert not (fields[9] in inodes and (table.startswith("udp") or fields[3] == "0A"))


def run():
    with tempfile.TemporaryDirectory(prefix="team-rpc-") as temp:
        temp = Path(temp)
        env = dict(os.environ, TEAM_REPO_ROOT=str(ROOT),
                   TEAM_BUILD_ROOT=str(ROOT / "build/original"),
                   TEAM_STATE_PATH=str(temp / "state.json"), TEAM_INVITE_TTL="2",
                   TEAM_BATTLE_SERVICE="", TEAM_POLICY_MODULE="team.policy")
        subprocess.run([str(ROOT / "start-server.sh"), "--build-only"], env=env, check=True,
                       stdout=subprocess.DEVNULL)
        binary = str(ROOT / "build/original/bin/skynet")
        config = str(ROOT / "lua/tests/team/integration_config.lua")
        for phase in ("seed", "recover", "rollback"):
            if phase == "rollback":
                env["TEAM_STATE_PATH"] = str(temp / "failed-state.json")
                Path(env["TEAM_STATE_PATH"] + ".tmp").mkdir()
            result = subprocess.run([binary, config], cwd="/tmp",
                                    env=dict(env, TEAM_TEST_PHASE=phase),
                                    capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            assert result.returncode == 0 and "TEAM_INTEGRATION_PASS" in output, output
            assert "TEAM_INTEGRATION_FAIL" not in output, output
            print(f"PASS: original Skynet RPC {phase}")

        env["TEAM_STATE_PATH"] = str(temp / "launcher-state.json")
        env["TEAM_DATA_DIR"] = str(temp)
        log_path = temp / "launcher.log"
        with log_path.open("w") as log:
            proc = subprocess.Popen([str(ROOT / "start-server.sh")], cwd="/tmp", env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 8
                while "LOGIN_SERVICE_READY .login" not in log_path.read_text():
                    assert proc.poll() is None, log_path.read_text()
                    assert time.monotonic() < deadline, log_path.read_text()
                    time.sleep(0.02)
                assert_no_listeners(proc.pid)
                assert not (temp / "accounts.lua").exists()
                second = subprocess.run([str(ROOT / "start-server.sh")], env=env,
                                        capture_output=True, timeout=5)
                assert second.returncode != 0 and b"already owns" in second.stderr
                print("PASS: launcher starts .team and .login without listening sockets or account files; state lock enforced")
            finally:
                proc.terminate()
                proc.wait(timeout=5)


if __name__ == "__main__":
    run()
