#!/usr/bin/env bash
#
# Launch a backend (amuled or amule) with External Connections (EC) enabled
# plus amuleapi, in an ISOLATED config so you can develop the web frontend
# without touching your real ~/.aMule.
#
# amuleapi serves the frontend itself (via StaticRoot) on the same origin as
# the API, so there is no separate static server and no proxy: just open the
# amuleapi URL. You edit HTML/JS/CSS in this repo and reload the browser.
#
# What it does:
#   - Uses its own config directory (default ~/.aMule-dev).
#   - On first run, generates the config headlessly (with amuled) so EC can be
#     enabled before launching, avoiding the first-run wizard.
#   - Enables External Connections (EC) with a password.
#   - Sets amuleapi's admin/guest web passwords.
#   - Points amuleapi's StaticRoot at the repo frontend (src/webapi/static).
#   - Starts the chosen backend in the background and amuleapi in the
#     foreground. Ctrl+C stops both.
#
# Usage:
#   tools/run-dev.sh            # amuled backend (default) + amuleapi
#   tools/run-dev.sh amuled     # same, explicit
#   tools/run-dev.sh amule      # amule GUI backend + amuleapi (needs a display)
#   tools/run-dev.sh stop       # stop the dev processes
#
# Optional environment variables:
#   BACKEND      Backend to launch: amuled | amule (def. amuled)
#   CONFIG_DIR   Isolated config directory (def. ~/.aMule-dev)
#   EC_PASS      EC password backend<->amuleapi (def. amuledev)
#   ADMIN_PASS   Web admin login password (def. admin)
#   GUEST_PASS   Web guest login password (def. guest)
#   HTTP_PORT    amuleapi / web HTTP port (def. 4713)
#   EC_PORT      Backend EC port (def. 4712)
#   BUILD_DIR    Build dir for freshly compiled binaries (def. <repo>/build)
#   STATIC_DIR   Static bundle to serve (def. <repo>/src/webapi/static)

set -euo pipefail

# tools/ and static/ are siblings under webapi/: webapi/ -> src/ -> repo
STATIC_DIR_DEFAULT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../static" && pwd)"
REPO_DIR="$(cd "$STATIC_DIR_DEFAULT/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build}"
STATIC_DIR="${STATIC_DIR:-$STATIC_DIR_DEFAULT}"
CONFIG_DIR="${CONFIG_DIR:-$HOME/.aMule-dev}"
EC_PASS="${EC_PASS:-amuledev}"
ADMIN_PASS="${ADMIN_PASS:-admin}"
GUEST_PASS="${GUEST_PASS:-guest}"
HTTP_PORT="${HTTP_PORT:-4713}"
EC_PORT="${EC_PORT:-4712}"

# Prefer freshly compiled binaries; otherwise fall back to the system (PATH).
pick_bin() {
	local name="$1" candidate="$2"
	if [[ -x "$candidate" ]]; then echo "$candidate"
	elif command -v "$name" >/dev/null 2>&1; then command -v "$name"
	else echo ""; fi
}
AMULE="$(pick_bin amule "$BUILD_DIR/src/amule")"
AMULED="$(pick_bin amuled "$BUILD_DIR/src/amuled")"
AMULEAPI="$(pick_bin amuleapi "$BUILD_DIR/src/webapi/amuleapi")"

md5hex() {  # uppercase hex md5 (the format amule.conf stores)
	if command -v python3 >/dev/null 2>&1; then
		python3 -c "import hashlib,sys;print(hashlib.md5(sys.argv[1].encode()).hexdigest().upper())" "$1"
	else
		printf '%s' "$1" | md5sum | cut -d' ' -f1 | tr 'a-f' 'A-F'
	fi
}

stop_all() {
	# Match by command line (the config dir is isolated => safe).
	pkill -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null || true
	pkill -f "amule -c $CONFIG_DIR"  2>/dev/null || true
	pkill -f "amuled -c $CONFIG_DIR" 2>/dev/null || true
	echo "==> Dev processes stopped."
}

port_in_use() {  # true (0) if something is already listening on 127.0.0.1:$1
	if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then exec 3>&- 3<&-; return 0; fi
	return 1
}

# --- argument parsing ---
BACKEND="${BACKEND:-amuled}"
case "${1:-}" in
	stop)          stop_all; exit 0 ;;
	amule|amuled)  BACKEND="$1" ;;
	"")            ;;
	*)             echo "ERROR: unknown argument '$1' (expected: amule | amuled | stop)"; exit 1 ;;
esac

# --- binary checks ---
if [[ -z "$AMULEAPI" ]]; then
	echo "ERROR: amuleapi not found (looked in $BUILD_DIR/src/webapi/amuleapi and PATH)."
	echo "       Build it with the BUILD_AMULEAPI CMake option, e.g.:"
	echo "           cmake -DBUILD_AMULEAPI=ON -B build && cmake --build build"
	exit 1
fi

if [[ "$BACKEND" == "amuled" && -z "$AMULED" ]]; then
	echo "ERROR: amuled not found (looked in $BUILD_DIR/src/amuled and PATH)."
	echo "       Build it with the BUILD_DAEMON CMake option, e.g.:"
	echo "           cmake -DBUILD_DAEMON=ON -B build && cmake --build build"
	exit 1
fi
if [[ "$BACKEND" == "amule" ]]; then
	if [[ -z "$AMULE" ]]; then
		echo "ERROR: amule not found (looked in $BUILD_DIR/src/amule and PATH)."
		echo "       Build it with the BUILD_MONOLITHIC CMake option (on by default), e.g.:"
		echo "           cmake -DBUILD_MONOLITHIC=ON -B build && cmake --build build"
		exit 1
	fi
	if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
		echo "ERROR: no display (DISPLAY/WAYLAND_DISPLAY). The amule backend is a GUI and needs a desktop."
		echo "       Use the default amuled backend instead: tools/run-dev.sh amuled"
		exit 1
	fi
fi

# --- port availability (fail fast before touching any config) ---
for entry in "EC:$EC_PORT" "HTTP:$HTTP_PORT"; do
	label="${entry%%:*}"; port="${entry##*:}"
	if port_in_use "$port"; then
		echo "ERROR: port $port ($label) is already in use."
		echo "       Something is already listening on it — a previous 'tools/run-dev.sh',"
		echo "       your real aMule/amuleapi, or another service. Stop it first:"
		echo "           tools/run-dev.sh stop"
		echo "       or pick free ports:  EC_PORT=14712 HTTP_PORT=14713 tools/run-dev.sh"
		exit 1
	fi
done

echo "==> backend : $BACKEND"
echo "==> amuleapi: $AMULEAPI"
echo "==> config  : $CONFIG_DIR (isolated)"
echo "==> frontend: $STATIC_DIR"

mkdir -p "$CONFIG_DIR"

# --- first run: generate amule.conf (headless, with amuled) ---
if [[ ! -f "$CONFIG_DIR/amule.conf" ]]; then
	echo "==> Generating initial configuration…"
	if [[ -n "$AMULED" ]]; then
		timeout 10 "$AMULED" -c "$CONFIG_DIR" -o -i >/dev/null 2>&1 || true
		pkill -f "amuled -c $CONFIG_DIR" 2>/dev/null || true
	fi
fi

echo "==> Enabling External Connections (EC) in amule.conf"
EC_MD5="$(md5hex "$EC_PASS")"
python3 - "$CONFIG_DIR/amule.conf" "$EC_MD5" "$EC_PORT" <<'PY' 2>/dev/null || true
import sys, re, os
conf, md5, ecport = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(conf).read() if os.path.exists(conf) else "[ExternalConnect]\nAcceptExternalConnections=1\nECPort=%s\nECPassword=%s\n" % (ecport, md5)
s = re.sub(r'AcceptExternalConnections=\d', 'AcceptExternalConnections=1', s)
s = re.sub(r'ECPassword=.*', 'ECPassword=' + md5, s)
s = re.sub(r'ECPort=.*', 'ECPort=' + ecport, s)
open(conf, 'w').write(s)
PY

# --- amuleapi passwords and config ---
echo "==> Setting amuleapi passwords (admin/guest)"
"$AMULEAPI" --config-dir="$CONFIG_DIR" --set-admin-pass="$ADMIN_PASS" >/dev/null 2>&1 || true
"$AMULEAPI" --config-dir="$CONFIG_DIR" --set-guest-pass="$GUEST_PASS" >/dev/null 2>&1 || true

if [[ ! -f "$CONFIG_DIR/amuleapi.conf" ]]; then
	timeout 3 "$AMULEAPI" --config-dir="$CONFIG_DIR" --password="$EC_PASS" --http-port="$HTTP_PORT" >/dev/null 2>&1 || true
	pkill -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null || true
fi
echo "==> Pointing StaticRoot at the repo frontend"
python3 - "$CONFIG_DIR/amuleapi.conf" "$STATIC_DIR" "$HTTP_PORT" <<'PY' 2>/dev/null || true
import sys, re, os
conf, front, port = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(conf).read() if os.path.exists(conf) else \
    "[Server]\nBindAddress=127.0.0.1\nPort=%s\nAllowCORS=0\nStaticRoot=\n" % port
if 'StaticRoot=' in s:
    s = re.sub(r'StaticRoot=.*', 'StaticRoot=' + front, s)
else:
    s = s.replace('[Server]\n', '[Server]\nStaticRoot=' + front + '\n', 1)
s = re.sub(r'^Port=.*', 'Port=' + port, s, count=1, flags=re.M)
open(conf, 'w').write(s)
PY

# --- launch ---
trap stop_all EXIT INT TERM

if [[ "$BACKEND" == "amule" ]]; then
	echo "==> Opening the amule window (log: $CONFIG_DIR/amule.log)"
	"$AMULE" -c "$CONFIG_DIR" >"$CONFIG_DIR/amule.log" 2>&1 &
else
	echo "==> Starting amuled in the background (log: $CONFIG_DIR/amuled.log)"
	"$AMULED" -c "$CONFIG_DIR" -o >"$CONFIG_DIR/amuled.log" 2>&1 &
fi

# Wait for the backend to open the EC port.
for _ in $(seq 1 40); do
	if (exec 3<>"/dev/tcp/127.0.0.1/$EC_PORT") 2>/dev/null; then exec 3>&- 3<&-; break; fi
	sleep 0.5
done

cat <<EOF

============================================================
  aMule web dev environment up
  Backend: $BACKEND
  Web:     http://127.0.0.1:$HTTP_PORT/
  Login:   admin -> '$ADMIN_PASS'   |   guest -> '$GUEST_PASS'
  EC:      127.0.0.1:$EC_PORT (pass '$EC_PASS')
  Logs:    $CONFIG_DIR/$BACKEND.log
  To stop: Ctrl+C  (or 'tools/run-dev.sh stop')
============================================================

EOF

echo "==> Starting amuleapi in the foreground…"
"$AMULEAPI" --config-dir="$CONFIG_DIR" --password="$EC_PASS" --http-port="$HTTP_PORT"
