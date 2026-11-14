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
  -u, --update              Force re-check and download of the latest CI build
  --uninstall               Remove GNU-Upscaler / OptiScaler from the target game
  -h, --help                Show this help message

${BOLD}Examples:${RESET}
  $0 \"/home/user/Games/Stellar Blade\"
  $0 --proxy=dxgi.dll --fsr4 --fg \"/path/to/game\"
  $0 --optifg \"~/Games/Cyberpunk 2077\"
  $0 --uninstall \"/path/to/game\""
    exit 0
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

    # 1. Unreal Engine check: <Game>/Binaries/Win64
    local ue_bin
    ue_bin="$(find "$target" -maxdepth 3 -type d -path "*/Binaries/Win64" 2>/dev/null | head -n 1 || true)"
    if [[ -n "$ue_bin" && -d "$ue_bin" ]]; then
        echo "$ue_bin"
        return 0
    fi

    # 2. CDPR / REDengine check: bin/x64
    if [[ -d "$target/bin/x64" ]]; then
        echo "$target/bin/x64"
        return 0
    fi

    # 3. Direct executable in the folder root
    if compgen -G "$target/*.exe" > /dev/null 2>&1; then
        echo "$target"
        return 0
    fi

    # 4. Search for a Shipping .exe or main .exe up to depth 3
    local found_exe
    found_exe="$(find "$target" -maxdepth 3 -type f -name "*Shipping.exe" -not -path "*/Engine/*" 2>/dev/null | head -n 1 || true)"
    if [[ -z "$found_exe" ]]; then
        found_exe="$(find "$target" -maxdepth 3 -type f -name "*.exe" \
            -not -path "*/Engine/*" -not -name "*CrashReport*" -not -name "*DirectX*" 2>/dev/null | head -n 1 || true)"
    fi

    if [[ -n "$found_exe" ]]; then
        dirname "$found_exe"
        return 0
    fi

    # Fallback to the provided directory
    echo "$target"
}

# Prompt interactively if target not provided
if [[ -z "$TARGET_PATH" ]]; then
    # Check if current working directory looks like a game
    CWD="$(pwd)"
    if compgen -G "$CWD/*.exe" > /dev/null 2>&1 || [[ -d "$CWD/Binaries/Win64" ]] || [[ -d "$CWD/bin/x64" ]]; then
        echo -e "${BOLD}Current directory appears to be a game folder:${RESET}"
        echo "  $CWD"
        read -r -p "Install GNU-Upscaler into this folder? [Y/n]: " reply
        reply="${reply:-y}"
        if [[ "$reply" =~ ^[Yy]$ ]]; then
            TARGET_PATH="$CWD"
        fi
    fi
fi

if [[ -z "$TARGET_PATH" ]]; then
    echo -e "${BOLD}GNU-Upscaler Installer${RESET}"
    read -r -p "Enter path to game folder or executable: " TARGET_PATH
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

# ── Uninstall Mode ───────────────────────────────────────────────────────────

if [[ $UNINSTALL -eq 1 ]]; then
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

    # Remove OptiScaler directories
    rm -rf "$BIN_DIR/OptiScaler" "$BIN_DIR/D3D12_OptiScaler" "$BIN_DIR/D3D12_Optiscaler" "$BIN_DIR/Licenses"

    log_success "GNU-Upscaler has been completely removed from $BIN_DIR"
    exit 0
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

    # Parse asset URL and timestamp using python3
    local parse_out
    parse_out="$(python3 -c "
import json, sys
try:
    d = json.loads('''$release_json''')
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

    curl -sL --max-time 600 --fail --progress-bar -H "User-Agent: gnu-upscaler-installer" \
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

# 3. Copy companion DLLs into game bin directory
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
path = '$INI_TARGET'
section = '$section'
key = '$key'
val = '$value'

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
    # insert under section
    idx = -1
    for i, l in enumerate(new_lines):
        if l.strip() == f'[{section}]':
            idx = i
            break
    if idx != -1:
        new_lines.insert(idx + 1, f'{key}={val}\n')

with open(path, 'w', encoding='utf-8') as f:
    f.writelines(new_lines)
" 2>/dev/null || true
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
    ini_set "FrameGen" "FGOutput" "auto"

    if [[ $CONFIG_OPTIFG -eq 1 ]]; then
        log_info "Configuring INI: Route = OptiFG (Universal upscaler framegen)"
        ini_set "FrameGen" "FGInput" "upscaler"
        ini_set "HUDFix" "HUDFix" "true"
        ini_set "FrameGen" "DisableHudless" "false"
    else
        ini_set "FrameGen" "FGInput" "auto"
    fi
fi

echo ""
log_success "GNU-Upscaler installed successfully!"
echo -e "${DIM}Target:${RESET} ${BIN_DIR}/${PROXY_CHOICE}"
echo -e "${DIM}Config:${RESET} ${INI_TARGET}"
echo ""
echo -e "${BOLD}Tip for Linux / Proton:${RESET}"
echo "If the game does not load the proxy DLL automatically, ensure WINEDLLOVERRIDES is set:"
echo -e "  ${CYAN}WINEDLLOVERRIDES=\"${PROXY_CHOICE%.dll}=n,b\" %command%${RESET}"
echo ""
