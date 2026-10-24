#!/usr/bin/env python3
"""Verify ID-only login against the production bootstrap in original Skynet."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def run():
    with tempfile.TemporaryDirectory(prefix="login-rpc-") as temp:
        temp = Path(temp)
        env = dict(os.environ, TEAM_REPO_ROOT=str(ROOT),
                   TEAM_BUILD_ROOT=str(ROOT / "build/original"),
                   TEAM_STATE_PATH=str(temp / "state.json"), TEAM_INVITE_TTL="60",
                   TEAM_BATTLE_SERVICE="", TEAM_POLICY_MODULE="team.policy")
        subprocess.run([str(ROOT / "start-server.sh"), "--build-only"],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        for phase in ("seed", "recover", "rollback"):
            if phase == "rollback":
                env["TEAM_STATE_PATH"] = str(temp / "failed-state.json")
                Path(env["TEAM_STATE_PATH"] + ".tmp").mkdir()
            result = subprocess.run(
                [str(ROOT / "build/original/bin/skynet"),
                 str(ROOT / "lua/tests/login/integration_config.lua")],
                cwd="/tmp", env=dict(env, LOGIN_TEST_PHASE=phase),
                capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            assert result.returncode == 0 and "LOGIN_INTEGRATION_PASS" in output, output
            assert "LOGIN_INTEGRATION_FAIL" not in output, output
            print(f"PASS: original Skynet login {phase}")


if __name__ == "__main__":
    run()
