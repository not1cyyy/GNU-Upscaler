#!/usr/bin/env bash
# ==============================================================================
# GNU-Upscaler Linux Installer
# Automatically fetches the latest MSVC build from GitHub Actions, unpacks it,
# detects the game's executable directory, and sets up OptiScaler.
# ==============================================================================

set -eo pipefail

REPO="not1cyyy/GNU-Upscaler"
API_URL="https://api.github.com/repos/${REPO}/releases/tags/latest"
CACHE_ROOT="${HOME}/.cache/gnu-upscaler"
CACHE_DIR="${CACHE_ROOT}/gnu-upscaler-latest"
STAMP_FILE="${CACHE_DIR}/build.stamp"

DEFAULT_PROXY="dxgi.dll"
PROXY_CHOICE="$DEFAULT_PROXY"
FORCE_UPDATE=0
UNINSTALL=0
TARGET_PATH=""

# Configuration preset flags
CONFIG_UPSCALER=""
CONFIG_FG=0
CONFIG_OPTIFG=0
CONFIG_GAQ=0
CONFIG_XEFG=0
CONFIG_MFG=""
FETCH_EXTRAS=""
AUTO_STEAM=0

SUPPORTED_PROXIES=("dxgi.dll" "winmm.dll" "version.dll" "d3d12.dll" "dbghelp.dll" "wininet.dll" "winhttp.dll")

# Colors for terminal output
BOLD="\033[1m"
GREEN="\033[0;32m"
CYAN="\033[0;36m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
DIM="\033[2m"
RESET="\033[0m"

log_info()    { echo -e "${CYAN}${BOLD}[+]${RESET} $*"; }
log_success() { echo -e "${GREEN}${BOLD}[✓]${RESET} $*"; }
log_warn()    { echo -e "${YELLOW}${BOLD}[!]${RESET} $*"; }
log_error()   { echo -e "${RED}${BOLD}[✗]${RESET} $*" >&2; }

show_help() {
    echo -e "${BOLD}GNU-Upscaler Linux Installer${RESET}

${BOLD}Usage:${RESET}
  $0 [OPTIONS] [GAME_PATH]

${BOLD}Arguments:${RESET}
  GAME_PATH                 Path to the game directory or main executable.
                            If omitted, checks the current folder or prompts interactively.

${BOLD}Options:${RESET}
  --proxy=<name>            Target DLL proxy filename (default: ${DEFAULT_PROXY})
                            Supported: dxgi.dll, winmm.dll, version.dll,
                                       d3d12.dll, dbghelp.dll, wininet.dll, winhttp.dll
  --fsr4                    Configure OptiScaler.ini for FSR 4 (Ffx) upscaling
  --fsr3                    Configure OptiScaler.ini for FSR 3.1 upscaling
  --xess                    Configure OptiScaler.ini for Intel XeSS upscaling
  --dlss                    Configure OptiScaler.ini for NVIDIA DLSS upscaling
  --fg                      Enable Frame Generation (auto DLSS-G / FSR-FG route)
  --optifg                  Enable OptiFG (Universal frame generation with HUD fix)
  --xefg                    Set Frame Generation output to XeFG
  --mfg=<mult>              Enable XeFG Multi-Frame Generation (2x, 3x, 4x, 6x, 10x)
  --gaq                     Enable GNU Adaptive Quality (GAQ Auto-DRS)
  --fetch-extras[=filter]   Download FSR4 INT8/FP8 variants into custom/ (default: recommended, or 'all' / tag filter)
  --auto-steam              Automatically inject WINEDLLOVERRIDES into Steam localconfig.vdf (with .bak backup)
  -u, --update              Force re-check and download of the latest CI build
  --uninstall               Remove GNU-Upscaler / OptiScaler from the target game
  -h, --help                Show this help message

${BOLD}Examples:${RESET}
  $0 \"/home/user/Games/Stellar Blade\"
  $0 --proxy=dxgi.dll --fsr4 --fg \"/path/to/game\"
  $0 --optifg --xefg --mfg=6x \"~/Games/Cyberpunk 2077\"
  $0 --uninstall \"/path/to/game\""
    exit 0
}

prompt_read() {
    local prompt_text="$1"
    local var_name="$2"
    if [[ -r /dev/tty ]]; then
        read -r -p "$prompt_text" "$var_name" < /dev/tty
    else
        read -r -p "$prompt_text" "$var_name"
    fi
}

check_dependencies() {
    local missing=()
    for cmd in curl python3; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    if ! command -v bsdtar >/dev/null 2>&1 && ! command -v unzip >/dev/null 2>&1; then
        missing+=("bsdtar or unzip")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required dependencies: ${missing[*]}"
        exit 1
    fi
}

validate_proxy() {
    local candidate="$1"
    for p in "${SUPPORTED_PROXIES[@]}"; do
        if [[ "$p" == "$candidate" ]]; then
            return 0
        fi
    done
    log_error "Unsupported proxy DLL: '$candidate'"
    echo "Supported proxies: ${SUPPORTED_PROXIES[*]}" >&2
    exit 1
}

# ── Argument Parsing ─────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            show_help
            ;;
        -u|--update)
            FORCE_UPDATE=1
            shift
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        --proxy=*)
            PROXY_CHOICE="${1#*=}"
            shift
            ;;
        --proxy)
            if [[ -z "${2:-}" || "$2" == -* ]]; then
                log_error "--proxy requires a DLL filename argument"
                exit 1
            fi
            PROXY_CHOICE="$2"
            shift 2
            ;;
        --fsr4)
            CONFIG_UPSCALER="fsr4"
            shift
            ;;
        --fsr3)
            CONFIG_UPSCALER="fsr3"
            shift
            ;;
        --xess)
            CONFIG_UPSCALER="xess"
            shift
            ;;
        --dlss)
            CONFIG_UPSCALER="dlss"
            shift
            ;;
        --fg)
            CONFIG_FG=1
            shift
            ;;
        --optifg)
            CONFIG_OPTIFG=1
            CONFIG_FG=1
            shift
            ;;
        --xefg)
            CONFIG_XEFG=1
            CONFIG_FG=1
            shift
            ;;
        --mfg=*)
            CONFIG_MFG="${1#*=}"
            CONFIG_XEFG=1
            CONFIG_FG=1
            shift
            ;;
        --gaq)
            CONFIG_GAQ=1
            shift
            ;;
        --fetch-extras)
            FETCH_EXTRAS="recommended"
            shift
            ;;
        --fetch-extras=*)
            FETCH_EXTRAS="${1#*=}"
            shift
            ;;
        --auto-steam)
            AUTO_STEAM=1
            shift
            ;;
        -*)
            log_error "Unknown option: $1"
            echo "Run '$0 --help' for usage."
            exit 1
            ;;
        *)
            if [[ -z "$TARGET_PATH" ]]; then
                TARGET_PATH="$1"
            else
                log_error "Multiple target paths specified ('$TARGET_PATH' and '$1')"
                exit 1
            fi
            shift
            ;;
    esac
done

check_dependencies
validate_proxy "$PROXY_CHOICE"

if [[ -n "$CONFIG_MFG" ]]; then
    case "${CONFIG_MFG,,}" in
        2x|3x|4x|6x|10x) ;;
        *)
            log_error "Unsupported --mfg multiplier: '$CONFIG_MFG' (expected 2x, 3x, 4x, 6x, or 10x)"
            exit 1
            ;;
    esac
fi

# ── Target Resolution ────────────────────────────────────────────────────────

resolve_game_bin_dir() {
    local raw="$1"
    # Expand tilde
    raw="${raw/#\~/$HOME}"
    local target
    target="$(realpath "$raw" 2>/dev/null || echo "$raw")"

    if [[ ! -e "$target" ]]; then
        log_error "Path does not exist: $raw"
        return 1
    fi

    # If it's a file, return its parent directory
    if [[ -f "$target" ]]; then
        dirname "$target"
        return 0
    fi

    # 1. Unreal Engine check: <Game>/Binaries/Win64 (supporting nested paths like Client/WindowsNoEditor/HT/Binaries/Win64)
    local ue_bin=""
    while IFS= read -r dir; do
        if find "$dir" -maxdepth 1 -type f -iname "*.exe" \
            -not -iname "*Crash*" -not -iname "*Helper*" -not -iname "ACE-*" 2>/dev/null | grep -q .; then
            ue_bin="$dir"
            break
        fi
        [[ -z "$ue_bin" ]] && ue_bin="$dir"
    done < <(find "$target" -maxdepth 6 -type d -path "*/Binaries/Win64" -not -path "*/Engine/*" 2>/dev/null | sort)
    if [[ -n "$ue_bin" && -d "$ue_bin" ]]; then
        echo "$ue_bin"
        return 0
    fi

    # 2. CDPR / REDengine check: bin/x64
    if [[ -d "$target/bin/x64" ]]; then
        echo "$target/bin/x64"
        return 0
    fi

    # 3. Search for a game .exe in deeper subdirectories before falling back to a root launcher .exe
    local found_exe=""
    found_exe="$(find "$target" -maxdepth 6 -type f \( -iname "*Win64-Shipping.exe" -o -iname "*Shipping.exe" \) \
        -not -path "*/Engine/*" -not -path "*/AntiCheatExpert/*" 2>/dev/null | head -n 1 || true)"
    if [[ -z "$found_exe" ]]; then
        found_exe="$(find "$target" -mindepth 2 -maxdepth 6 -type f -iname "*.exe" \
            -not -path "*/Engine/*" -not -path "*/AntiCheatExpert/*" -not -path "*/_Redist/*" \
            -not -iname "*Crash*" -not -iname "*DirectX*" -not -iname "*Launcher*" \
            -not -iname "*Update*" -not -iname "*Browser*" -not -iname "*WebBooster*" \
            -not -iname "*Helper*" -not -iname "*unins*" -not -iname "*setup*" \
            -not -iname "*vc_redist*" 2>/dev/null | head -n 1 || true)"
    fi

    if [[ -n "$found_exe" ]]; then
        dirname "$found_exe"
        return 0
    fi

    # 4. Direct executable in the folder root
    if compgen -G "$target/*.exe" > /dev/null 2>&1; then
        echo "$target"
        return 0
    fi

    # Fallback to the provided directory
    echo "$target"
}

discover_steam_games() {
    python3 -c '
import os, re

candidates = [
    os.path.expanduser("~/.local/share/Steam/steamapps/libraryfolders.vdf"),
    os.path.expanduser("~/.steam/steam/steamapps/libraryfolders.vdf"),
    os.path.expanduser("~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/libraryfolders.vdf"),
]

lib_paths = []
for vdf in candidates:
    if os.path.isfile(vdf):
        try:
            with open(vdf, "r", encoding="utf-8", errors="ignore") as f:
                for m in re.finditer(r"\"path\"\s+\"([^\"]+)\"", f.read()):
                    p = m.group(1).replace("\\\\", "/")
                    if os.path.isdir(os.path.join(p, "steamapps", "common")):
                        lib_paths.append(os.path.join(p, "steamapps", "common"))
        except Exception:
            pass

games = []
seen = set()
skip = {"Steamworks Shared", "Proton", "SteamLinuxRuntime"}
for common in lib_paths:
    try:
        for entry in sorted(os.listdir(common)):
            if any(entry.startswith(s) for s in skip):
                continue
            full = os.path.join(common, entry)
            if os.path.isdir(full) and full not in seen:
                seen.add(full)
                games.append(full)
    except Exception:
        pass

for g in games:
    print(g)
' 2>/dev/null || true
}

# Prompt interactively if target not provided
if [[ -z "$TARGET_PATH" ]]; then
    # Check if current working directory looks like a game
    CWD="$(pwd)"
    if compgen -G "$CWD/*.exe" > /dev/null 2>&1 || [[ -d "$CWD/Binaries/Win64" ]] || [[ -d "$CWD/bin/x64" ]] || \
       find "$CWD" -maxdepth 6 -type d -path "*/Binaries/Win64" -not -path "*/Engine/*" 2>/dev/null | grep -q .; then
        echo -e "${BOLD}Current directory appears to be a game folder:${RESET}"
        echo "  $CWD"
        prompt_read "Install GNU-Upscaler into this folder? [Y/n]: " reply
        reply="${reply:-y}"
        if [[ "$reply" =~ ^[Yy]$ ]]; then
            TARGET_PATH="$CWD"
        fi
    fi
fi

if [[ -z "$TARGET_PATH" ]]; then
    echo -e "${BOLD}GNU-Upscaler Installer${RESET}"
    mapfile -t STEAM_GAMES < <(discover_steam_games)
    if [[ ${#STEAM_GAMES[@]} -gt 0 ]]; then
        echo -e "${CYAN}Detected Steam games:${RESET}"
        for i in "${!STEAM_GAMES[@]}"; do
            printf "  ${BOLD}[%d]${RESET} %s\n" "$((i + 1))" "$(basename "${STEAM_GAMES[$i]}")"
        done
        echo -e "  ${BOLD}[0]${RESET} Enter a custom path"
        prompt_read "Select a game [0-${#STEAM_GAMES[@]}]: " selection
        if [[ "$selection" =~ ^[0-9]+$ ]] && [[ "$selection" -ge 1 ]] && [[ "$selection" -le ${#STEAM_GAMES[@]} ]]; then
            TARGET_PATH="${STEAM_GAMES[$((selection - 1))]}"
        fi
    fi
fi

if [[ -z "$TARGET_PATH" ]]; then
    prompt_read "Enter path to game folder or executable: " TARGET_PATH
    if [[ -z "$TARGET_PATH" ]]; then
        log_error "No target path provided. Aborting."
        exit 1
    fi
fi

BIN_DIR="$(resolve_game_bin_dir "$TARGET_PATH")"
if [[ -z "$BIN_DIR" || ! -d "$BIN_DIR" ]]; then
    log_error "Could not resolve a valid game directory for: $TARGET_PATH"
    exit 1
fi

log_info "Game binary directory resolved to:"
echo -e "    ${BOLD}${BIN_DIR}${RESET}"

# ── Check for Existing Installation ──────────────────────────────────────────

is_already_installed() {
    [[ -f "$BIN_DIR/OptiScaler.dll" ]] || \
    [[ -f "$BIN_DIR/OptiScaler.ini" ]] || \
    [[ -d "$BIN_DIR/OptiScaler" ]]
}

do_uninstall() {
    echo ""
    log_info "Uninstalling GNU-Upscaler from: $BIN_DIR"
    
    # Remove known proxies
    for proxy in dxgi.dll winmm.dll version.dll d3d12.dll dbghelp.dll wininet.dll winhttp.dll OptiScaler.asi; do
        if [[ -f "$BIN_DIR/$proxy" ]]; then
            rm -f "$BIN_DIR/$proxy"
            log_info "Removed proxy: $proxy"
        fi
    done

    # Remove artifacts
    for art in OptiScaler.dll OptiScaler.ini OptiScaler.log fakenvapi.dll fakenvapi.ini fakenvapi.log \
               dlssg_to_fsr3_amd_is_better.dll; do
        if [[ -f "$BIN_DIR/$art" ]]; then
            rm -f "$BIN_DIR/$art"
            log_info "Removed artifact: $art"
        fi
    done

    # Remove companion DLLs
    for dll in amd_fidelityfx_*.dll libxess_fg.dll libxell.dll; do
        rm -f "$BIN_DIR"/$dll 2>/dev/null || true
    done

    # Remove OptiScaler directories & OptiPatcher plugin (preserve unrelated user plugins)
    rm -rf "$BIN_DIR/OptiScaler" "$BIN_DIR/D3D12_OptiScaler" "$BIN_DIR/D3D12_Optiscaler" "$BIN_DIR/Licenses" "$BIN_DIR/custom"
    if [[ -f "$BIN_DIR/plugins/OptiPatcher.asi" ]]; then
        rm -f "$BIN_DIR/plugins/OptiPatcher.asi"
        log_info "Removed plugin: OptiPatcher.asi"
    fi
    rmdir "$BIN_DIR/plugins" 2>/dev/null || true

    log_success "GNU-Upscaler has been completely removed from $BIN_DIR"
    patch_steam_launch_options "remove" "${PROXY_CHOICE%.dll}" || true
    exit 0
}

if [[ $UNINSTALL -eq 1 ]]; then
    do_uninstall
fi

# If already installed and no specific config flags were given, prompt for action
if is_already_installed && [[ -z "$CONFIG_UPSCALER" && $CONFIG_FG -eq 0 && $CONFIG_GAQ -eq 0 && $FORCE_UPDATE -eq 0 ]]; then
    echo ""
    log_warn "GNU-Upscaler is already installed in this directory."
    echo -e "Choose an action:"
    echo -e "  ${BOLD}[1]${RESET} Reinstall or update files (default)"
    echo -e "  ${BOLD}[2]${RESET} Uninstall GNU-Upscaler"
    echo -e "  ${BOLD}[3]${RESET} Cancel"
    prompt_read "Enter choice [1-3] (default 1): " choice
    choice="${choice:-1}"

    case "$choice" in
        2)
            do_uninstall
            ;;
        3)
            echo "Operation cancelled."
            exit 0
            ;;
        *)
            log_info "Proceeding with reinstall / update..."
            ;;
    esac
fi

# ── Fetch & Cache GNU-Upscaler Build ─────────────────────────────────────────

mkdir -p "$CACHE_ROOT"

fetch_latest_build() {
    log_info "Checking GitHub for latest GNU-Upscaler build (${REPO})..."
    local release_json
    release_json="$(curl -sL --max-time 20 -H "User-Agent: gnu-upscaler-installer" "$API_URL" || true)"

    if [[ -z "$release_json" ]] || echo "$release_json" | grep -q '"message": "Not Found"'; then
        if [[ -f "$CACHE_DIR/OptiScaler.dll" ]]; then
            log_warn "Could not reach GitHub release. Using previously cached build."
            return 0
        else
            log_error "Could not reach GitHub and no local cache was found."
            log_error "Check your internet connection or verify the repository is accessible."
            exit 1
        fi
    fi

    # Parse asset URL and timestamp using python3 via stdin
    local parse_out
    parse_out="$(printf '%s' "$release_json" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    for a in d.get('assets', []):
        if a['name'].endswith('.zip'):
            print(a['browser_download_url'])
            print(a.get('updated_at', ''))
            sys.exit(0)
except Exception:
    sys.exit(1)
" 2>/dev/null || true)"

    local asset_url
    local asset_stamp
    asset_url="$(echo "$parse_out" | sed -n '1p')"
    asset_stamp="$(echo "$parse_out" | sed -n '2p')"

    if [[ -z "$asset_url" ]]; then
        if [[ -f "$CACHE_DIR/OptiScaler.dll" ]]; then
            log_warn "No .zip asset found in release. Using previously cached build."
            return 0
        else
            log_error "No .zip asset found in GitHub release: $API_URL"
            exit 1
        fi
    fi

    local cached_stamp=""
    if [[ -f "$STAMP_FILE" ]]; then
        cached_stamp="$(cat "$STAMP_FILE" 2>/dev/null || true)"
    fi

    # Check if we already have the latest build
    if [[ $FORCE_UPDATE -eq 0 && -f "$CACHE_DIR/OptiScaler.dll" && -n "$asset_stamp" && "$cached_stamp" == "$asset_stamp" ]]; then
        log_success "Cached build is up to date (${asset_stamp})"
        return 0
    fi

    # Download new build
    log_info "Downloading latest build: $(basename "$asset_url") ..."
    local zip_file="${CACHE_ROOT}/download.zip"
    rm -f "$zip_file"

    curl -L --max-time 600 --fail --progress-bar -H "User-Agent: gnu-upscaler-installer" \
         -o "$zip_file" "$asset_url"

    if [[ ! -f "$zip_file" ]] || [[ "$(stat -c%s "$zip_file" 2>/dev/null || stat -f%z "$zip_file" 2>/dev/null)" -lt 1048576 ]]; then
        log_error "Download failed or archive is corrupt."
        exit 1
    fi

    log_info "Unpacking into cache (${CACHE_DIR}) ..."
    rm -rf "$CACHE_DIR"
    mkdir -p "$CACHE_DIR"

    if command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf "$zip_file" -C "$CACHE_DIR"
    elif command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$zip_file" -d "$CACHE_DIR"
    else
        log_error "Neither 'bsdtar' nor 'unzip' is installed. Please install one to unpack archives."
        exit 1
    fi

    rm -f "$zip_file"

    if [[ ! -f "$CACHE_DIR/OptiScaler.dll" ]]; then
        log_error "Extraction finished but OptiScaler.dll was not found."
        exit 1
    fi

    # Write build stamp
    if [[ -n "$asset_stamp" ]]; then
        echo "$asset_stamp" > "$STAMP_FILE"
    fi

    log_success "GNU-Upscaler build ready in cache."
}

detect_steam_appid() {
    local target_dir="${1:-$BIN_DIR}"
    python3 -c '
import glob, os, re, sys

cur = os.path.realpath(sys.argv[1])
install_dir = None
steamapps_dir = None

while True:
    parent = os.path.dirname(cur)
    grandparent = os.path.dirname(parent)
    if os.path.basename(parent) == "common" and os.path.basename(grandparent) == "steamapps":
        install_dir = os.path.basename(cur)
        steamapps_dir = grandparent
        break
    if parent == cur:
        break
    cur = parent

if not install_dir or not steamapps_dir:
    sys.exit(0)

exact_match = None
ci_match = None
for acf in sorted(glob.glob(os.path.join(steamapps_dir, "appmanifest_*.acf"))):
    try:
        with open(acf, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        m_id = re.search(r"\"appid\"\s+\"(\d+)\"", txt, re.IGNORECASE)
        m_dir = re.search(r"\"installdir\"\s+\"([^\"]+)\"", txt, re.IGNORECASE)
        if not m_id or not m_dir:
            continue
        if m_dir.group(1) == install_dir:
            exact_match = m_id.group(1)
            break
        if m_dir.group(1).lower() == install_dir.lower() and not ci_match:
            ci_match = m_id.group(1)
    except Exception:
        pass

if exact_match or ci_match:
    print(exact_match or ci_match)
' "$target_dir" 2>/dev/null || true
}

patch_steam_launch_options() {
    local action="${1:-add}"
    local proxy_stem="${2:-${PROXY_CHOICE%.dll}}"

    local appid
    appid="$(detect_steam_appid "$BIN_DIR")"
    [[ -z "$appid" ]] && return 0

    local vdf_files=()
    mapfile -t vdf_files < <(python3 -c '
import glob, os
patterns = [
    os.path.expanduser("~/.local/share/Steam/userdata/*/config/localconfig.vdf"),
    os.path.expanduser("~/.steam/steam/userdata/*/config/localconfig.vdf"),
    os.path.expanduser("~/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata/*/config/localconfig.vdf"),
]
seen = set()
for pat in patterns:
    for p in sorted(glob.glob(pat)):
        rp = os.path.realpath(p)
        if os.path.isfile(rp) and rp not in seen:
            seen.add(rp)
            print(rp)
' 2>/dev/null || true)

    [[ ${#vdf_files[@]} -eq 0 ]] && return 0

    if [[ "$action" == "add" ]]; then
        if [[ $AUTO_STEAM -eq 0 ]]; then
            local reply="n"
            if [[ -t 0 || -r /dev/tty ]]; then
                prompt_read "Detected Steam AppID ${appid}. Inject WINEDLLOVERRIDES=\"${proxy_stem}=n,b\" into Steam launch options? [Y/n]: " reply
                reply="${reply:-y}"
            fi
            if [[ ! "$reply" =~ ^[Yy]$ ]]; then
                return 0
            fi
        fi

        if command -v pgrep >/dev/null 2>&1 && pgrep -x steam >/dev/null 2>&1; then
            log_warn "Steam appears to be running; restart Steam (or close Steam before editing if it overwrites localconfig.vdf on exit)."
        fi
    elif [[ $AUTO_STEAM -eq 0 ]]; then
        local reply="y"
        if [[ -t 0 || -r /dev/tty ]]; then
            prompt_read "Detected Steam AppID ${appid}. Remove WINEDLLOVERRIDES=${proxy_stem}=n,b from Steam launch options? [Y/n]: " reply
            reply="${reply:-y}"
        fi
        if [[ ! "$reply" =~ ^[Yy]$ ]]; then
            return 0
        fi
    fi

    for vdf in "${vdf_files[@]}"; do
        local result
        result="$(python3 -c '
import re, shutil, sys

vdf_path, appid, stem, mode = sys.argv[1:5]

with open(vdf_path, "r", encoding="utf-8", errors="ignore") as f:
    text = f.read()

def vdf_unescape(s):
    return s.replace("\\\\", "\\").replace("\\\"", "\"")

def vdf_escape(s):
    return s.replace("\\", "\\\\").replace("\"", "\\\"")

def merge_launch_options(existing, stem, mode):
    existing = existing.strip()
    override_token = f"{stem}=n,b"
    env_pat = re.compile(r"WINEDLLOVERRIDES=(?:\"([^\"]*)\"|([^\s]+))")
    m = env_pat.search(existing)

    if mode == "remove":
        if not m:
            return existing
        raw_val = m.group(1) if m.group(1) is not None else m.group(2)
        parts = [p for p in raw_val.split(";") if p and p.split("=", 1)[0].lower() != stem.lower()]
        if parts:
            replacement = "WINEDLLOVERRIDES=\"" + ";".join(parts) + "\""
            updated = existing[:m.start()] + replacement + existing[m.end():]
        else:
            updated = (existing[:m.start()] + " " + existing[m.end():]).strip()
            updated = re.sub(r"\s{2,}", " ", updated)
            if updated == "%command%":
                updated = ""
        return updated

    # mode == "add"
    if m:
        raw_val = m.group(1) if m.group(1) is not None else m.group(2)
        parts = [p for p in raw_val.split(";") if p]
        replaced = False
        new_parts = []
        for p in parts:
            if p.split("=", 1)[0].lower() == stem.lower():
                if not replaced:
                    new_parts.append(override_token)
                    replaced = True
            else:
                new_parts.append(p)
        if not replaced:
            new_parts.append(override_token)
        replacement = "WINEDLLOVERRIDES=\"" + ";".join(new_parts) + "\""
        updated = existing[:m.start()] + replacement + existing[m.end():]
        if "%command%" not in updated:
            updated = f"{updated} %command%".strip()
        return updated

    prefix = f"WINEDLLOVERRIDES=\"{override_token}\""
    if not existing:
        return f"{prefix} %command%"
    if "%command%" in existing:
        return f"{prefix} {existing}"
    return f"{prefix} %command% {existing}"

lines = text.splitlines(keepends=True)
newline = "\r\n" if (lines and lines[0].endswith("\r\n")) else "\n"

stack = []
pending_key = None
qstr_re = re.compile(r"\"((?:[^\"\\]|\\.)*)\"")

apps_path = ["userlocalconfigstore", "software", "valve", "steam", "apps"]
target_path = apps_path + [appid.lower()]

apps_end_idx = None
appid_open_idx = None
appid_indent = "\t\t\t\t\t"
launch_line_idx = None
launch_old_val = ""
launch_indent = "\t\t\t\t\t\t"

for idx, raw_line in enumerate(lines):
    s = raw_line.strip()
    if not s or s.startswith("//"):
        continue

    if s == "{":
        if pending_key is not None:
            stack.append(pending_key.lower())
            pending_key = None
        else:
            stack.append("")
        if stack == target_path and appid_open_idx is None:
            appid_open_idx = idx
            m_ind = re.match(r"^(\s*)", raw_line)
            if m_ind:
                appid_indent = m_ind.group(1)
                launch_indent = appid_indent + "\t"
        continue

    if s == "}":
        if stack == target_path and apps_end_idx is None:
            pass
        if stack == apps_path and apps_end_idx is None:
            apps_end_idx = idx
        if stack:
            stack.pop()
        pending_key = None
        continue

    tokens = [vdf_unescape(m.group(1)) for m in qstr_re.finditer(s)]
    if len(tokens) == 1:
        pending_key = tokens[0]
    elif len(tokens) >= 2:
        pending_key = None
        key_l = tokens[0].lower()
        val = tokens[1]
        if stack == target_path and key_l == "launchoptions":
            launch_line_idx = idx
            launch_old_val = val
            m_ind = re.match(r"^(\s*)", raw_line)
            if m_ind:
                launch_indent = m_ind.group(1)

if mode == "remove" and launch_line_idx is None:
    print("unchanged")
    sys.exit(0)

new_val = merge_launch_options(launch_old_val, stem, mode)
if launch_line_idx is not None and new_val == launch_old_val:
    print("unchanged")
    sys.exit(0)
if launch_line_idx is None and mode == "add" and not new_val:
    print("unchanged")
    sys.exit(0)

escaped_new = vdf_escape(new_val)
if launch_line_idx is not None:
    lines[launch_line_idx] = f"{launch_indent}\"LaunchOptions\"\t\t\"{escaped_new}\"{newline}"
elif appid_open_idx is not None:
    lines.insert(appid_open_idx + 1, f"{launch_indent}\"LaunchOptions\"\t\t\"{escaped_new}\"{newline}")
elif apps_end_idx is not None:
    block = [
        f"{appid_indent}\"{appid}\"{newline}",
        f"{appid_indent}{{{newline}",
        f"{appid_indent}\t\"LaunchOptions\"\t\t\"{escaped_new}\"{newline}",
        f"{appid_indent}}}{newline}",
    ]
    lines[apps_end_idx:apps_end_idx] = block
else:
    print("not_found")
    sys.exit(0)

shutil.copy2(vdf_path, vdf_path + ".bak")
with open(vdf_path, "w", encoding="utf-8", errors="ignore") as f:
    f.writelines(lines)
print(f"updated:{new_val}")
' "$vdf" "$appid" "$proxy_stem" "$action" 2>/dev/null || true)"

        case "$result" in
            updated:*)
                local opts="${result#updated:}"
                log_success "Updated Steam AppID ${appid} LaunchOptions in ${vdf} (backup: ${vdf}.bak)"
                if [[ -n "$opts" ]]; then
                    echo -e "  ${DIM}LaunchOptions:${RESET} ${CYAN}${opts}${RESET}"
                fi
                ;;
            not_found)
                log_warn "Could not find Software->Valve->Steam->apps section in ${vdf}"
                ;;
        esac
    done
}

fetch_latest_build

# ── Installation to Game ─────────────────────────────────────────────────────

echo ""
log_info "Installing GNU-Upscaler into: ${BIN_DIR}"

# 1. Copy proxy DLL (e.g. dxgi.dll)
log_info "Installing proxy DLL as: ${BOLD}${PROXY_CHOICE}${RESET}"
cp -f "$CACHE_DIR/OptiScaler.dll" "$BIN_DIR/$PROXY_CHOICE"
cp -f "$CACHE_DIR/OptiScaler.dll" "$BIN_DIR/OptiScaler.dll"

# 2. Copy backend directory
if [[ -d "$CACHE_DIR/OptiScaler" ]]; then
    log_info "Copying backend directory: OptiScaler/ ..."
    cp -rf "$CACHE_DIR/OptiScaler" "$BIN_DIR/"
fi

# 2b. Copy custom providers directory (FSR4 variants, etc.) if present
if [[ -d "$CACHE_DIR/custom" ]]; then
    log_info "Copying custom upscaler providers (custom/)..."
    cp -rf "$CACHE_DIR/custom" "$BIN_DIR/"
fi

fetch_extras_providers() {
    log_info "Fetching latest FSR4 INT8 & FP8 variants from Optiscaler-Client..."
    local custom_dir="$BIN_DIR/custom"
    local tmp_extras="${CACHE_ROOT}/tmp_extras"
    mkdir -p "$custom_dir" "$tmp_extras"

    local repos=("Optiscaler-Client/OptiScaler-Extras:INT8" "Optiscaler-Client/OptiScaler-Extras-FP8:FP8")
    for entry in "${repos[@]}"; do
        local repo="${entry%%:*}"
        local suffix="${entry##*:}"
        local json
        json="$(curl -sL --max-time 20 -H "User-Agent: gnu-upscaler-installer" \
            "https://api.github.com/repos/${repo}/releases?per_page=20" || true)"
        [[ -z "$json" ]] && continue

        while IFS='|' read -r tag asset_name asset_url; do
            [[ -z "$asset_url" ]] && continue
            local safe_tag="${tag//[^a-zA-Z0-9._-]/_}"
            local variant="${safe_tag}_${suffix}"
            local target_ffx="$custom_dir/$variant/ffx"
            if [[ -d "$target_ffx" ]] && compgen -G "$target_ffx/*.dll" > /dev/null 2>&1; then
                continue
            fi

            local archive="$tmp_extras/$asset_name"
            local extract_dir="$tmp_extras/$variant"
            log_info "Downloading ${variant} (${asset_name})..."
            if ! curl -L --max-time 120 --fail --progress-bar -H "User-Agent: gnu-upscaler-installer" -o "$archive" "$asset_url"; then
                log_warn "Failed to download ${asset_name}"
                continue
            fi

            mkdir -p "$extract_dir"
            if [[ "$asset_name" == *.7z ]]; then
                if command -v 7z >/dev/null 2>&1; then
                    7z x -y "-o$extract_dir" "$archive" >/dev/null 2>&1 || true
                elif command -v bsdtar >/dev/null 2>&1; then
                    bsdtar -xf "$archive" -C "$extract_dir" 2>/dev/null || true
                else
                    log_warn "Skipping ${asset_name}: install 'p7zip' or 'libarchive-tools' (bsdtar) to extract .7z"
                    continue
                fi
            else
                if command -v bsdtar >/dev/null 2>&1; then
                    bsdtar -xf "$archive" -C "$extract_dir" 2>/dev/null || true
                elif command -v unzip >/dev/null 2>&1; then
                    unzip -q -o "$archive" -d "$extract_dir" 2>/dev/null || true
                fi
            fi

            local found_dlls
            found_dlls="$(find "$extract_dir" -type f -iname "*.dll" 2>/dev/null || true)"
            if [[ -n "$found_dlls" ]]; then
                mkdir -p "$target_ffx"
                while IFS= read -r dll_path; do
                    cp -f "$dll_path" "$target_ffx/"
                done <<< "$found_dlls"
            else
                log_warn "No DLLs extracted from ${asset_name} (if .7z failed, install p7zip)"
            fi
        done < <(printf '%s' "$json" | python3 -c "
import json, sys
mode = sys.argv[1].lower()
try:
    releases = json.load(sys.stdin)
    for idx, r in enumerate(releases):
        tag = r.get('tag_name', '')
        tl = tag.lower()
        if mode == 'recommended':
            if idx != 0 and 'lossy' not in tl and 'exact' not in tl:
                continue
        elif mode != 'all':
            if mode not in tl:
                continue
        for a in r.get('assets', []):
            name = a.get('name', '')
            if name.endswith(('.zip', '.7z')):
                print(f\"{tag}|{name}|{a['browser_download_url']}\")
                break
except Exception:
    pass
" "$FETCH_EXTRAS" 2>/dev/null)
    done
    rm -rf "$tmp_extras"
}

if [[ -n "$FETCH_EXTRAS" ]]; then
    fetch_extras_providers
fi

# 3. Copy plugins directory (OptiPatcher.asi, etc.)
if [[ -d "$CACHE_DIR/plugins" ]]; then
    log_info "Installing plugins (OptiPatcher.asi)..."
    mkdir -p "$BIN_DIR/plugins"
    cp -rf "$CACHE_DIR/plugins/"* "$BIN_DIR/plugins/"
elif [[ -f "$CACHE_DIR/OptiPatcher.asi" ]]; then
    log_info "Installing plugin (OptiPatcher.asi)..."
    mkdir -p "$BIN_DIR/plugins"
    cp -f "$CACHE_DIR/OptiPatcher.asi" "$BIN_DIR/plugins/OptiPatcher.asi"
fi

# 4. Copy companion DLLs into game bin directory
copy_companion() {
    local dll="$1"
    if [[ -f "$CACHE_DIR/$dll" ]]; then
        cp -f "$CACHE_DIR/$dll" "$BIN_DIR/$dll"
    elif [[ -f "$CACHE_DIR/OptiScaler/$dll" ]]; then
        cp -f "$CACHE_DIR/OptiScaler/$dll" "$BIN_DIR/$dll"
    fi
}

log_info "Bundling FidelityFX & XeSS companion libraries..."
for dll in amd_fidelityfx_loader_dx12.dll \
           amd_fidelityfx_upscaler_dx12.dll \
           amd_fidelityfx_framegeneration_dx12.dll \
           amd_fidelityfx_loader_vk.dll \
           amd_fidelityfx_upscaler_vk.dll \
           amd_fidelityfx_framegeneration_vk.dll \
           libxess_fg.dll \
           libxell.dll; do
    copy_companion "$dll"
done

# 4. OptiScaler.ini configuration
INI_TARGET="$BIN_DIR/OptiScaler.ini"
if [[ -f "$INI_TARGET" ]]; then
    log_warn "Existing OptiScaler.ini detected — preserving user configuration."
else
    if [[ -f "$CACHE_DIR/OptiScaler.ini" ]]; then
        log_info "Creating default OptiScaler.ini..."
        cp -f "$CACHE_DIR/OptiScaler.ini" "$INI_TARGET"
    fi
fi

# Helper to modify an INI key
ini_set() {
    local section="$1"
    local key="$2"
    local value="$3"
    python3 -c "
import sys
path, section, key, val = sys.argv[1:5]

lines = []
try:
    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
except Exception:
    pass

sec_found = False
key_set = False
new_lines = []
current_sec = ''

for l in lines:
    stripped = l.strip()
    if stripped.startswith('[') and stripped.endswith(']'):
        current_sec = stripped[1:-1].strip()
        if current_sec == section:
            sec_found = True
    elif current_sec == section and '=' in stripped and not stripped.startswith((';', '#')):
        k = stripped.split('=', 1)[0].strip()
        if k.lower() == key.lower():
            new_lines.append(f'{key}={val}\n')
            key_set = True
            continue
    new_lines.append(l)

if not sec_found:
    new_lines.append(f'\n[{section}]\n{key}={val}\n')
elif not key_set:
    idx = -1
    for i, l in enumerate(new_lines):
        if l.strip() == f'[{section}]':
            idx = i
            break
    if idx != -1:
        new_lines.insert(idx + 1, f'{key}={val}\n')

with open(path, 'w', encoding='utf-8') as f:
    f.writelines(new_lines)
" "$INI_TARGET" "$section" "$key" "$value" 2>/dev/null || true
}

# Apply requested preset configurations
if [[ -n "$CONFIG_UPSCALER" ]]; then
    case "$CONFIG_UPSCALER" in
        fsr4)
            log_info "Configuring INI: Upscaler = FSR 4"
            ini_set "Upscalers" "Dx12Upscaler" "ffx"
            ini_set "Upscalers" "Dx11Upscaler" "ffx_12"
            ini_set "Upscalers" "VulkanUpscaler" "ffx"
            ini_set "FSR" "UpscalerIndex" "auto"
            ;;
        fsr3)
            log_info "Configuring INI: Upscaler = FSR 3.1"
            ini_set "Upscalers" "Dx12Upscaler" "ffx"
            ini_set "Upscalers" "Dx11Upscaler" "ffx_12"
            ini_set "Upscalers" "VulkanUpscaler" "ffx"
            ini_set "FSR" "UpscalerIndex" "1"
            ;;
        xess)
            log_info "Configuring INI: Upscaler = XeSS"
            ini_set "Upscalers" "Dx12Upscaler" "xess"
            ini_set "Upscalers" "Dx11Upscaler" "xess_12"
            ini_set "Upscalers" "VulkanUpscaler" "xess"
            ;;
        dlss)
            log_info "Configuring INI: Upscaler = DLSS"
            ini_set "Upscalers" "Dx12Upscaler" "dlss"
            ini_set "Upscalers" "Dx11Upscaler" "dlss"
            ini_set "Upscalers" "VulkanUpscaler" "dlss"
            ;;
    esac
fi

if [[ $CONFIG_FG -eq 1 ]]; then
    log_info "Configuring INI: Enabling Frame Generation"
    ini_set "FrameGen" "Enabled" "true"
    if [[ $CONFIG_XEFG -eq 1 ]]; then
        log_info "Configuring INI: FGOutput = XeFG"
        ini_set "FrameGen" "FGOutput" "xefg"
    else
        ini_set "FrameGen" "FGOutput" "auto"
    fi

    if [[ -n "$CONFIG_MFG" ]]; then
        local_count=""
        case "${CONFIG_MFG,,}" in
            2x)  local_count="1" ;;
            3x)  local_count="2" ;;
            4x)  local_count="3" ;;
            6x)  local_count="5" ;;
            10x) local_count="9" ;;
        esac
        if [[ -n "$local_count" ]]; then
            log_info "Configuring INI: XeFG InterpolationCount = ${local_count} (${CONFIG_MFG,,})"
            ini_set "XeFG" "InterpolationCount" "$local_count"
        fi
    fi

    if [[ $CONFIG_OPTIFG -eq 1 ]]; then
        log_info "Configuring INI: Route = OptiFG (Universal upscaler framegen)"
        ini_set "FrameGen" "FGInput" "upscaler"
        ini_set "HUDFix" "HUDFix" "true"
        ini_set "FrameGen" "DisableHudless" "false"
    else
        ini_set "FrameGen" "FGInput" "auto"
    fi
fi

if [[ $CONFIG_GAQ -eq 1 ]]; then
    log_info "Configuring INI: Enabling GNU Adaptive Quality (GAQ Auto-DRS)"
    ini_set "GAQ" "Enabled" "true"
fi

echo ""
log_success "GNU-Upscaler installed successfully!"
echo -e "${DIM}Target:${RESET} ${BIN_DIR}/${PROXY_CHOICE}"
echo -e "${DIM}Config:${RESET} ${INI_TARGET}"
echo ""

patch_steam_launch_options "add" "${PROXY_CHOICE%.dll}"

echo -e "${BOLD}Tip for Linux / Proton:${RESET}"
echo "If the game does not load the proxy DLL automatically, ensure WINEDLLOVERRIDES is set:"
echo -e "  ${CYAN}WINEDLLOVERRIDES=\"${PROXY_CHOICE%.dll}=n,b\" %command%${RESET}"
echo ""
