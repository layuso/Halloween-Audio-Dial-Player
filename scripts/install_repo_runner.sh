#!/usr/bin/env bash
# One-time, interactive bootstrap on waymarker-build-01. Never put registration tokens in git.
set -euo pipefail
REPO_URL="https://github.com/layuso/Halloween-Audio-Dial-Player"
RUNNER_NAME="waymarker-build-01-halloween"
RUNNER_LABEL="halloween-dial-builder"
RUNNER_HOME="$HOME/actions-runner-halloween"
if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "aarch64" ]]; then
  echo "Expected ARM64 Linux build Pi, got $(uname -s) $(uname -m)" >&2; exit 1
fi
for tool in curl python3 tar sha256sum; do command -v "$tool" >/dev/null || { echo "Missing $tool"; exit 1; }; done
if [[ -e "$RUNNER_HOME/.runner" ]]; then
 echo "Runner is already configured: $RUNNER_HOME"; exit 0
fi
mkdir -p "$RUNNER_HOME"
cd "$RUNNER_HOME"
if [[ ! -x ./config.sh ]]; then
  echo "Finding newest official ARM64 GitHub Actions runner..."
  mapfile -t asset < <(python3 - <<'PY'
import json,urllib.request
url="https://api.github.com/repos/actions/runner/releases/latest"
req=urllib.request.Request(url,headers={"User-Agent":"halloween-dial-runner-bootstrap"})
data=json.load(urllib.request.urlopen(req,timeout=25))
matches=[a for a in data["assets"] if a["name"].startswith("actions-runner-linux-arm64-") and a["name"].endswith(".tar.gz")]
if len(matches)!=1:raise SystemExit("Unable to identify official linux-arm64 runner")
asset=matches[0]
print(asset["browser_download_url"])
print(asset.get("digest") or "")
PY
  )
  if [[ "${#asset[@]}" != 2 || "${asset[1]}" != sha256:* ]]; then
    echo "Official GitHub release has no SHA256 digest; refusing unverified install." >&2; exit 1
  fi
  curl -fSL --retry 3 "${asset[0]}" -o runner.tar.gz
  printf '%s  %s\n' "${asset[1]#sha256:}" runner.tar.gz | sha256sum --check --status
  tar xzf runner.tar.gz
  rm runner.tar.gz
fi
echo
echo "Generate a one-hour registration token at:"
echo "$REPO_URL/settings/actions/runners/new"
echo "Select Linux / ARM64; copy only the registration TOKEN from the displayed config command."
read -r -s -p "Registration token: " TOKEN
echo
if [[ -z "$TOKEN" ]]; then echo "Registration token empty" >&2; exit 1; fi
./config.sh --unattended --replace --url "$REPO_URL" \
 --token "$TOKEN" --name "$RUNNER_NAME" --labels "$RUNNER_LABEL" --work "_work_halloween"
unset TOKEN
echo "Runner configured. Installing systemd service for user $(id -un) ..."
if command -v sudo >/dev/null; then
 sudo ./svc.sh install "$(id -un)"
 sudo ./svc.sh start
 sudo ./svc.sh status
else
 echo "No sudo available; start this runner manually with: cd $RUNNER_HOME && ./run.sh"
fi
echo "Verify runner ONLINE at $REPO_URL/settings/actions/runners"
