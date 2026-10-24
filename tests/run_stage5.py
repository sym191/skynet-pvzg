"""Run the shared Lua network contract with a real stdin pipe."""
import subprocess
import sys

result = subprocess.run(sys.argv[1:], input=b"stage5 stdin\n", capture_output=True, timeout=60)
output = result.stdout.decode(errors="replace")
errors = result.stderr.decode(errors="replace")
if result.returncode or "STAGE5_OK" not in output or "STAGE5_FAILED" in output:
    sys.exit(f"stage5 failed ({result.returncode})\n{output}\n{errors}")
print(output, end="")
