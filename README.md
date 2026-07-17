# Display Layout Editor

A focused drag-and-drop display layout editor. It provides a sparse, spatial workflow without turning display placement into a full settings dashboard.

![Display Layout preview](docs/preview.png)

Display Layout is an independent project. NVIDIA is not affiliated with, does not endorse, and does not own this project. No NVIDIA source code, artwork, trademarks, or other proprietary assets are included, and no infringement of or ownership claim over NVIDIA's works is intended or implied.

## Compositor support

Display Layout selects a backend from the current desktop automatically. The supported compositor families are:

| Compositor            | Backend/API                               |
| --------------------- | ----------------------------------------- |
| niri                  | `niri msg` IPC                            |
| Sway                  | `swaymsg` IPC                             |
| Hyprland              | `hyprctl` IPC                             |
| River, Wayfire, labwc | wlroots output management via `wlr-randr` |
| GNOME Shell           | Mutter DisplayConfig via `gdctl`          |
| KDE Plasma            | KScreen via `kscreen-doctor`              |

`wlr-randr` is included in the Nix package. The other tools are supplied by their compositor or desktop session. Explicit backend names are `niri`, `sway`, `hyprland`, `wlr`, `gnome`, and `kscreen`; `river`, `wayfire`, `labwc`, `mutter`, `kde`, and `plasma` are accepted aliases.

## Features

- Logical-pixel, to-scale display previews
- Drag-and-drop placement with proximity snapping for edges and centerlines
- Animated alignment guides while displays line up horizontally or vertically
- Reset, close, and atomic user-confirmed apply workflow
- Numbered output badges plus simultaneous, per-output identification overlays
- Dark and light themes
- Pixel- or percentage-based default window dimensions
- Configurable font, font size, and snap distance
- Small C codebase and runtime closure

## Run

```console
nix run github:johnrichardrinehart/display-layout
```

Or build locally:

```console
nix build
./result/bin/display-layout
```

## Configuration

Copy [`config.example.ini`](config.example.ini) to `$XDG_CONFIG_HOME/display-layout/config.ini` (normally `~/.config/display-layout/config.ini`). The application never creates or rewrites its configuration automatically.

Dimensions independently accept pixels or percentages:

```ini
width = 1060px
height = 87%
font-size = 16
# font = /path/to/a/monospace-font.ttf
theme = system
snap-distance = 14
identify-duration-ms = 2000
backend = auto
```

Use `theme = dark`, `light`, or `system`. System mode checks common desktop and GTK preferences and falls back to dark. An explicit config can be selected with `--config PATH`, and a backend can be selected with `--backend NAME`.

## Backend architecture

The generic application owns rendering, interaction, configuration, and the display layout model:

- `src/main.c` — backend-independent UI and interaction
- `src/model.h` — backend-neutral display data
- `src/backend.h` / `src/backend.c` — backend contract and selection
- `src/backend_niri.c` — niri IPC
- `src/backend_sway.c` — Sway IPC
- `src/backend_hyprland.c` — Hyprland IPC
- `src/backend_wlr.c` — wlroots output management
- `src/backend_gnome.c` — GNOME/Mutter integration
- `src/backend_kscreen.c` — KDE/KScreen integration
- `src/backend_common.c` — safe command execution, JSON helpers, and shared identification
- `tests/backend_test.c` — parser and backend-selection regression tests
- `tests/nixos/backend-matrix.nix` — NixOS VM render/apply matrix with a screenshot for every backend

A backend implements only `load`, `apply`, and lifecycle operations. Commands are invoked directly without shell interpolation, output is parsed in-process, and multi-output APIs are applied atomically where the compositor API permits it.

## Development

The flake follows the same dev-input partitioning used by the author's other projects. Consumer evaluation does not fetch development-only inputs.

```console
direnv allow
nix flake check
```

On x86-64, the flake check includes a NixOS VM test. It launches the packaged editor against contract fixtures for all six backend APIs, captures `backend-*.png` screenshots from the VM, activates Apply, and verifies that each compositor command receives the layout.

Development tooling uses:

- flake-parts
- treefmt-nix
- nix-direnv
- git-hooks.nix
- clang-format

## License

MIT. The vendored `jsmn` parser retains its own MIT license notice in `src/third_party/jsmn.h`.
