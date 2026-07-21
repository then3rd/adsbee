#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# SETUP.sh — bootstrap the ADSBee 1090 `just` workflow.
#
# Installs the tools the justfile drives:
#   • just    — the task runner itself
#   • Docker  — build.sh compiles all three firmwares in containers
#   • python3 — flash / console / health helper scripts
# Then configures shell completions and runs `just setup`
# (git submodules + version-sync pre-commit hook).
#
# Supports: Ubuntu/Debian, Arch Linux, macOS (Homebrew).
# Usage: bash SETUP.sh
# ---------------------------------------------------------------------------

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[setup]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

detect_os() {
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    elif [[ -f /etc/os-release ]]; then
        # shellcheck source=/dev/null
        source /etc/os-release
        case "$ID" in
            ubuntu|debian|pop|linuxmint) echo "debian" ;;
            arch|manjaro|endeavouros)    echo "arch"   ;;
            *) error "Unsupported distro: $ID. Add it to SETUP.sh or install just/Docker/python3 manually." ;;
        esac
    else
        error "Cannot detect OS. Install just, Docker and python3 manually."
    fi
}

detect_shell() {
    local shell_name
    shell_name="$(basename "${SHELL:-}")"
    if [[ -z "$shell_name" ]]; then
        shell_name="$(ps -p $PPID -o comm= 2>/dev/null | sed 's/^-//' || true)"
    fi
    echo "$shell_name"
}

# ─── just ──────────────────────────────────────────────────────────────────
install_just_macos() {
    command -v brew &>/dev/null || error "Homebrew not found. Install it from https://brew.sh then re-run."
    info "Installing just via Homebrew…"
    brew install just
}

install_just_binary() {
    # Prebuilt binary from GitHub releases — works on any arch (x86_64, aarch64, armv7).
    info "Installing just via prebuilt binary from GitHub releases…"
    local arch
    arch="$(uname -m)"
    case "$arch" in
        x86_64)  arch="x86_64" ;;
        aarch64) arch="aarch64" ;;
        armv7l)  arch="armv7" ;;
        *) error "No prebuilt just binary for arch '$arch'. Install cargo and run: cargo install just" ;;
    esac
    local tag
    tag="$(curl -fsSL https://api.github.com/repos/casey/just/releases/latest \
        | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": "\(.*\)".*/\1/')"
    [[ -z "$tag" ]] && error "Could not determine latest just release tag."
    local url="https://github.com/casey/just/releases/download/${tag}/just-${tag}-${arch}-unknown-linux-musl.tar.gz"
    info "Downloading just ${tag} for ${arch}…"
    local tmp
    tmp="$(mktemp -d)"
    curl -fsSL "$url" | tar -xz -C "$tmp"
    sudo install -m 0755 "$tmp/just" /usr/local/bin/just
    rm -rf "$tmp"
    info "just $(just --version) installed to /usr/local/bin/just"
}

install_just_debian() {
    info "Installing just via apt…"
    sudo apt-get update -qq
    if apt-cache show just &>/dev/null 2>&1; then
        sudo apt-get install -y just
    elif command -v snap &>/dev/null; then
        warn "'just' not in apt cache — falling back to snap."
        sudo snap install just --classic
    else
        warn "'just' not in apt or snap — falling back to prebuilt binary."
        install_just_binary
    fi
}

install_just_arch() {
    info "Installing just via pacman…"
    sudo pacman -Sy --noconfirm just
}

# ─── Docker ──────────────────────────────────────────────────────────────────
install_docker_macos() {
    if command -v docker &>/dev/null; then
        info "Docker already installed ($(docker --version))."
        return
    fi
    command -v brew &>/dev/null || \
        error "Homebrew not found. Install Docker Desktop manually: https://www.docker.com/products/docker-desktop."
    info "Installing Docker Desktop via Homebrew…"
    brew install --cask docker
    info "Docker Desktop installed. Launch it from Applications to start the daemon."
}

setup_docker_service() {
    # Rootless Docker (user-session service) takes priority — no sudo or group needed.
    if systemctl --user is-active --quiet docker 2>/dev/null; then
        info "Rootless Docker already running (user session)."
        return
    fi
    if systemctl --user is-enabled --quiet docker 2>/dev/null; then
        info "Starting rootless Docker (user session)…"
        systemctl --user start docker
        return
    fi

    if ! systemctl is-enabled --quiet docker 2>/dev/null; then
        info "Enabling Docker service…"
        sudo systemctl enable docker
    fi
    if ! systemctl is-active --quiet docker 2>/dev/null; then
        info "Starting Docker service…"
        sudo systemctl start docker
    else
        info "Docker service already running."
    fi

    if ! groups "$USER" | grep -qw docker; then
        info "Adding $USER to the docker group…"
        sudo usermod -aG docker "$USER"
        warn "Group change takes effect in a new shell session (or run: newgrp docker)."
    fi
}

install_docker_debian() {
    if command -v docker &>/dev/null; then
        info "Docker already installed ($(docker --version))."
    else
        info "Installing Docker (docker.io) via apt…"
        sudo apt-get update -qq
        sudo apt-get install -y docker.io docker-compose-v2
    fi
    setup_docker_service
}

install_docker_arch() {
    if command -v docker &>/dev/null; then
        info "Docker already installed ($(docker --version))."
    else
        info "Installing Docker via pacman…"
        sudo pacman -Sy --noconfirm docker docker-compose
    fi
    setup_docker_service
}

# ─── Python (flash / console / health helpers) ───────────────────────────────
install_python_macos() {
    if command -v python3 &>/dev/null; then
        info "python3 already installed ($(python3 --version))."
    else
        command -v brew &>/dev/null || error "Homebrew not found. Install python3 manually."
        info "Installing python3 via Homebrew…"
        brew install python
    fi
}

install_python_debian() {
    if command -v python3 &>/dev/null && python3 -m pip --version &>/dev/null; then
        info "python3 + pip already installed ($(python3 --version))."
    else
        info "Installing python3 + pip via apt…"
        sudo apt-get update -qq
        sudo apt-get install -y python3 python3-pip python3-serial
    fi
}

install_python_arch() {
    if command -v python3 &>/dev/null && python3 -m pip --version &>/dev/null; then
        info "python3 + pip already installed ($(python3 --version))."
    else
        info "Installing python3 + pip via pacman…"
        sudo pacman -Sy --noconfirm python python-pip python-pyserial
    fi
}

# ─── Shell completions ────────────────────────────────────────────────────────
setup_completions() {
    local shell_name
    shell_name="$(detect_shell)"
    info "Detected shell: $shell_name"

    case "$shell_name" in
        zsh)
            local comp_dir="$HOME/.zsh/completions"
            mkdir -p "$comp_dir"
            just --completions zsh > "$comp_dir/_just"
            info "Zsh completions written to $comp_dir/_just"
            if ! grep -q 'HOME/.zsh/completions' "${ZDOTDIR:-$HOME}/.zshrc" 2>/dev/null; then
                warn "Add the following to your .zshrc if completions aren't loading:"
                echo '    fpath=(~/.zsh/completions $fpath)'
                echo '    autoload -Uz compinit && compinit'
            fi
            ;;
        bash)
            local comp_dir="$HOME/.local/share/bash-completion/completions"
            mkdir -p "$comp_dir"
            just --completions bash > "$comp_dir/just"
            info "Bash completions written to $comp_dir/just"
            ;;
        *)
            warn "Unknown shell '$shell_name' — skipping completions. Run 'just --completions <shell>' manually."
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    local os
    os="$(detect_os)"
    info "Detected OS: $os"

    # 1. just
    if command -v just &>/dev/null; then
        info "just already installed ($(just --version))."
    else
        "install_just_${os}"
    fi

    # 2. Docker (build toolchain)
    "install_docker_${os}"

    # 3. python3 (helper scripts)
    "install_python_${os}"

    # 4. Shell completions
    if command -v just &>/dev/null; then
        setup_completions
    else
        warn "just not found after install — skipping completions."
    fi

    # 5. Bootstrap the repo via the justfile itself: submodules + version-sync hook.
    if command -v just &>/dev/null; then
        info "Running 'just setup' (git submodules + version-sync pre-commit hook)…"
        ( cd "$SCRIPT_DIR" && just setup )
    fi

    info "Done. Open a new shell for completions and docker-group changes to take effect."
    info "Next: 'just' for the menu, then 'just build' to compile firmware."
}

main "$@"
