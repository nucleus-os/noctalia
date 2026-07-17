# Noctalia

Noctalia is a native Wayland desktop shell for people who want a polished, configurable Linux desktop without stitching
together a separate bar, launcher, notification daemon, lock screen, wallpaper tool, and settings UI.

It provides the shell layer around your compositor: bars, widgets, dock, launcher, control center, notifications,
wallpaper, lock screen, session actions, clipboard history, OSDs, tray integration, and desktop widgets. The project is
built directly on Wayland with Vulkan 1.4 and Skia Graphite, with no Qt or GTK dependency, so the UI, rendering, configuration, and IPC model
are designed as one cohesive shell instead of a collection of unrelated panels and scripts.

> [!IMPORTANT]
> Noctalia v5 is currently in Beta. While the core features and architecture are stabilizing, you may still encounter occasional configuration or behavior adjustments as we prepare for the final release.

<p><br/></p>

<p align="center">
  <img src="https://assets.noctalia.dev/noctalia-logo.svg?v=2" alt="Noctalia Logo" style="width: 192px" />
</p>

<p align="center">
  <a href="https://docs.noctalia.dev/v5/getting-started/installation">
    <img
      src="https://img.shields.io/badge/Install_Noctalia-FFF59B?style=for-the-badge&labelColor=FFF59B"
      alt="Install Noctalia"
      style="height: 50px"
    />
  </a>
</p>

<p><br/></p>

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/commits">
    <img src="https://img.shields.io/github/last-commit/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=git&logoColor=070722&label=commit" alt="Last commit" />
  </a>
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://img.shields.io/github/stars/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=github&logoColor=070722" alt="GitHub stars" />
  </a>
  <a href="https://docs.noctalia.dev">
    <img src="https://img.shields.io/badge/docs-FFF59B?style=for-the-badge&logo=gitbook&logoColor=070722&labelColor=FFF59B" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-FFF59B?style=for-the-badge&labelColor=FFF59B&logo=discord&logoColor=070722" alt="Discord" />
  </a>
</p>

## Why Noctalia?

Most Wayland setups leave the desktop shell to a stack of small tools: one bar, another launcher, another notification
daemon, a lock screen, a wallpaper daemon, scripts for session actions, and separate config formats for each piece. That
can be flexible, but it also makes a complete desktop feel fragile and hard to keep visually consistent.

Noctalia solves that by providing one configurable shell layer that owns the common desktop surfaces and services while
still fitting into compositor-driven Wayland workflows. It is meant for users who want the control of a custom desktop
environment with fewer moving parts and a consistent UI.

## What It Includes

- Multi-monitor bars with configurable widgets, taskbar, workspaces, system tray, media, network, battery, brightness,
  weather, clipboard, and custom script-backed widgets.
- Dock, launcher, control center, notification toasts/history, wallpaper picker, OSD overlays, lock screen, session
  panel, and desktop widgets.
- TOML configuration with hot reload, GUI-managed overrides, theme/palette support, template application, and IPC for
  runtime control.
- Direct Wayland integration for layer-shell, session lock, idle behavior, clipboard, foreign toplevels, workspaces,
  fractional scaling, and compositor-specific workspace backends where needed.

## Wayland Compositor Support

Noctalia supports Wayland compositors that provide the layer-shell protocols it needs for shell surfaces. Workspace
integration works through compositor-native backends where needed, or through `ext-workspace-v1` on compositors that
implement it.

Current compositor integrations include Niri, Hyprland, Sway, Scroll, Mango, Labwc, Triad, dwl, and other compatible
Wayland compositors. Other compositors may run Noctalia but can have reduced workspace, window, output, or
session-action integration depending on the protocols and IPC they expose.

## Scope

Noctalia is a desktop shell, not a full desktop environment. It provides the visual and service layer around your
Wayland compositor: bars, panels, launcher, notifications, dock, lock screen, idle behavior, OSDs, theming, wallpapers,
desktop widgets, and multi-monitor shell surfaces.

Window management, tiling, file management, removable-drive mounting, and screen mirroring/casting belong to the
compositor, dedicated desktop applications, or system services. Display/login greeter support lives in the separate
[Noctalia Greeter](https://github.com/noctalia-dev/noctalia-greeter) project. Noctalia may integrate with those pieces
when useful, but it does not replace them.

The plugin system is available for user-installed extensions. Features that are useful to some users but not essential
to the core shell can live there: extra bar widgets, launcher providers, desktop widgets, panels, shortcuts, background
services, compositor-specific extras, hardware-specific controls, and third-party service integrations.

## Dependencies

Noctalia has one supported native ABI: the Nucleus Clang/libc++ toolchain, the
versioned Nucleus render SDK, the private libc++ dependency bundle, and the
codec-enabled CEF distribution. GCC, libstdc++, distro Skia, distro sdbus-c++,
distro libqalculate, and a CEF build with a different allocator/Skia contract
are unsupported even if they happen to link.

Generate the required artifacts before configuring:

```sh
git clone --recurse-submodules git@github.com:nucleus-os/nucleus.git nucleus-workspace
cd nucleus-workspace
tools/nucleus bootstrap

cd /path/to/noctalia
tools/bootstrap-nucleus-cpp-deps.sh
```

The patched, codec-enabled CEF m151 distribution must be present under
`~/.cache/nucleus/cef/dist/current`, or selected explicitly with
`CEF_SDK_PATH`. `just configure` checks the required SDK files and uses these
defaults; `NUCLEUS_TOOLCHAIN_ROOT`, `NUCLEUS_RENDER_SDK_PATH`,
`NUCLEUS_CPP_DEPS_PATH`, and `CEF_SDK_PATH` override them.

The following lists are only the remaining C/system build prerequisites. Do
not substitute their C++ sdbus-c++, libqalculate, toml++, Skia, or CEF packages.

### Arch

```sh
sudo pacman -S meson ninja cmake clang just \
  wayland wayland-protocols \
  vulkan-headers vulkan-icd-loader freetype2 fontconfig \
  libxkbcommon glib2 \
  libpipewire wireplumber polkit \
  pam curl libwebp libxml2 \
  md4c \
  nlohmann-json stb
```

### Fedora

```sh
sudo dnf install meson ninja-build cmake clang just \
  wayland-devel wayland-protocols-devel \
  vulkan-headers vulkan-loader-devel \
  freetype-devel fontconfig-devel \
  libxkbcommon-devel glib2-devel \
  pipewire-devel wireplumber-devel \
  pam-devel polkit-devel libcurl-devel libwebp-devel \
  libxml2-devel md4c-devel \
  json-devel stb_image_resize2-devel stb_image_write-devel
```

### openSUSE Tumbleweed / Slowroll

```sh
sudo zypper install meson ninja cmake clang just \
  wayland-devel wayland-protocols-devel \
  vulkan-devel \
  freetype2-devel fontconfig-devel \
  libxkbcommon-devel glib2-devel \
  pipewire-devel wireplumber-devel \
  pam-devel polkit-devel libcurl-devel libwebp-devel \
  libxml2-devel md4c-devel \
  nlohmann_json-devel stb-devel
```

### Debian / Ubuntu

```sh
sudo apt install meson ninja-build cmake clang just \
  libwayland-dev wayland-protocols \
  libvulkan-dev \
  libfreetype-dev libfontconfig-dev \
  libxkbcommon-dev libglib2.0-dev \
  libpipewire-0.3-dev libwireplumber-0.5-dev \
  libpam0g-dev libpolkit-agent-1-dev libpolkit-gobject-1-dev \
  libcurl4-openssl-dev libwebp-dev libxml2-dev \
  libmd4c-dev \
  nlohmann-json3-dev libstb-dev
```

### Void Linux

```sh
sudo xbps-install meson ninja pkg-config git \
  wayland-devel wayland-protocols Vulkan-Headers Vulkan-Loader \
  fontconfig-devel freetype-devel \
  libxkbcommon-devel pipewire-devel wireplumber-devel \
  libcurl-devel pam-devel libwebp-devel \
  basu-devel libmd4c-devel \
  json-c++ stb \
  polkit-devel libxml2-devel
```

Vendored dependencies, with no system package needed: `Luau`, `dr_wav`,
`fzy`, and Material Color Utilities.

System packages required beyond the Wayland/Vulkan stack: `libwebp` handles
thumbnail encoding; raster image decoding uses the Nucleus Skia SDK.
`libqalculate` powers the launcher calculator (arithmetic, unit and currency
conversion).

Polkit agent support requires development files that provide the `polkit-agent-1` and `polkit-gobject-1` pkg-config
modules. Some distros ship these in the runtime `polkit` package, while split-package distros use names such as
`polkit-devel`, `polkit-dev`, or `libpolkit-agent-1-dev` / `libpolkit-gobject-1-dev`.

Pipewire libraries/headers are sufficient to build Noctalia, but there is also a runtime requirement for the pipewire
daemon. Noctalia will abort startup if it can't connect to the daemon. If your distro splits the pipewire libraries
and daemon into separate packages, make sure you have both installed.

`upower` is an optional dependency used for battery and power device integration.

`ddcutil` is an optional dependency used for controlling monitor brightness.

`wtype` is an optional dependency used for clipboard auto-paste.

Sanitizer runtime packages are only needed for ASan/UBSan builds configured with `just configure asan`.

The sources are built as C++23 with the Nucleus Clang/libc++ toolchain. A
different system compiler is intentionally rejected because the linked Skia
and private C++ libraries use that ABI.

## Building and installing

Requires [just](https://github.com/casey/just) and [meson](https://mesonbuild.com/).

#### Release build

```sh
# Optimized release build in build-release/
just configure release
just build release

# Install the selected build mode. This does not build or reconfigure.
sudo just install release
```

Release builds are portable by default. For a machine-local build, enable native CPU optimizations after configuring:

```sh
meson configure build-release -Dnative_optimizations=true
just build release
```

Pass a prefix to `configure` to install somewhere other than `/usr/local`:

```sh
just configure release "$HOME/.local"
just build release
just install release
```

To remove files installed from a build directory, run `just uninstall release`. The `install` and `uninstall` recipes
require an explicit build mode so debug builds are not installed by accident.

#### Debug build

```sh
# Debug build in build-debug/ for local development and troubleshooting.
just configure
just build

# Test your local debug build with
just run
```

Unit tests are built automatically for debug builds and skipped for release builds. Build and run them with
`just test` (use `just test release` to force them on for a release build). Override the default with the meson
`-Dtests=enabled|disabled|auto` option.

Meson installs the binary and shipped assets using the normal prefix layout:

```text
/usr/local/bin/noctalia
/usr/local/share/noctalia/assets/...
```

Noctalia needs the shipped `assets/` tree at runtime. Copying only the `noctalia` binary is not enough.

Portable bundle layouts are also supported:

```text
bundle/
  noctalia
  assets/
```

```text
bundle/
  bin/noctalia
  share/noctalia/assets/
```

See [CONTRIBUTING.md](CONTRIBUTING.md#runtime-assets) for the full runtime asset lookup order.

## Configuration

A ready-to-use starting config with all defaults is at [example.toml](example.toml). The full configuration reference
lives in the [documentation site](https://docs.noctalia.dev/v5/).

## Contributing

Developer notes, architecture overview, code style, project layout, and debugging commands live in
[CONTRIBUTING.md](CONTRIBUTING.md).

Bug reports, fixes, documentation updates, themes, and configuration examples are welcome. For general help and design
discussion, join the community on [Discord](https://discord.noctalia.dev).

## Credits

Thank you to the [contributors](https://github.com/noctalia-dev/noctalia/graphs/contributors) and community
members who test Noctalia, report issues, share configurations, and help shape the project.

## Donations

Donations are appreciated but completely optional.

<p>
  <a href="https://www.buymeacoffee.com/noctalia">
    <img src="https://img.shields.io/badge/Buy_Me_a_Coffee-FFF59B?style=for-the-badge&logo=buymeacoffee&logoColor=070722&labelColor=FFF59B" alt="Buy Me a Coffee">
  </a>
  <a href="https://ko-fi.com/noctaliadev">
    <img src="https://img.shields.io/badge/Ko--fi-FFF59B?style=for-the-badge&logo=kofi&logoColor=070722&labelColor=FFF59B" alt="Ko-fi">
  </a>
</p>

## License

MIT License. See [LICENSE](LICENSE) for details.

## Star History

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://api.noctalia.dev/stars" alt="Star History" />
  </a>
</p>
