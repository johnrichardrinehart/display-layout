{ pkgs, package }:
let
  fixtures = pkgs.runCommand "display-layout-backend-fixtures" { } ''
    mkdir -p $out/bin
    cat > $out/bin/compositor-fixture <<'SH'
    #!${pkgs.runtimeShell}
    set -eu
    command="$(basename "$0")"
    case "$command:$*" in
      "niri:msg --json outputs")
        echo '{"DP-1":{"name":"DP-1","make":"Dell","model":"U2720Q","physical_size":[600,340],"logical":{"x":0,"y":0,"width":1920,"height":1080}},"HDMI-A-1":{"name":"HDMI-A-1","make":"BOE","model":"Laptop Panel","physical_size":[280,190],"logical":{"x":1920,"y":180,"width":1280,"height":800}}}' ;;
      "swaymsg:-r -t get_outputs")
        echo '[{"name":"DP-1","make":"Dell","model":"U2720Q","active":true,"physical_width":600,"physical_height":340,"rect":{"x":0,"y":0,"width":1920,"height":1080}},{"name":"HDMI-A-1","make":"BOE","model":"Laptop Panel","active":true,"physical_width":280,"physical_height":190,"rect":{"x":1920,"y":180,"width":1280,"height":800}}]' ;;
      "hyprctl:-j monitors")
        echo '[{"name":"DP-1","make":"Dell","model":"U2720Q","width":3840,"height":2160,"refreshRate":60.0,"x":0,"y":0,"scale":2.0,"transform":0,"physicalWidth":600,"physicalHeight":340,"disabled":false},{"name":"HDMI-A-1","make":"BOE","model":"Laptop Panel","width":1280,"height":800,"refreshRate":60.0,"x":1920,"y":180,"scale":1.0,"transform":0,"physicalWidth":280,"physicalHeight":190,"disabled":false}]' ;;
      "wlr-randr:--json")
        echo '[{"name":"DP-1","make":"Dell","model":"U2720Q","physical_size":{"width":600,"height":340},"enabled":true,"modes":[{"width":3840,"height":2160,"current":true}],"position":{"x":0,"y":0},"transform":"normal","scale":2.0},{"name":"HDMI-A-1","make":"BOE","model":"Laptop Panel","physical_size":{"width":280,"height":190},"enabled":true,"modes":[{"width":1280,"height":800,"current":true}],"position":{"x":1920,"y":180},"transform":"normal","scale":1.0}]' ;;
      "kscreen-doctor:--json")
        echo '{"outputs":[{"name":"DP-1","enabled":true,"pos":{"x":0,"y":0},"size":{"width":3840,"height":2160},"sizeMM":{"width":600,"height":340},"scale":2.0},{"name":"HDMI-A-1","enabled":true,"pos":{"x":1920,"y":180},"size":{"width":1280,"height":800},"sizeMM":{"width":280,"height":190},"scale":1.0}]}' ;;
      "gdctl:show")
        printf 'Monitors:\n  Monitor DP-1 (Dell U2720Q)\n    Vendor: Dell\n    Product: U2720Q\n    Current mode\n      3840x2160@60.000\n  Monitor HDMI-A-1 (BOE Laptop Panel)\n    Vendor: BOE\n    Product: Laptop Panel\n    Current mode\n      1280x800@60.000\n\nLogical monitors:\n  Logical monitor #1\n    Position: (0, 0)\n    Scale: 2.0\n    Transform: normal\n    Primary: yes\n    Monitors: (1)\n      DP-1 (Dell U2720Q)\n  Logical monitor #2\n    Position: (1920, 180)\n    Scale: 1.0\n    Transform: normal\n    Primary: no\n    Monitors: (1)\n      HDMI-A-1 (BOE Laptop Panel)\n' ;;
      "niri:msg output DP-1 position set -- 0 0" | \
      "swaymsg:output DP-1 pos 0 0" | \
      "hyprctl:keyword monitor DP-1,3840x2160@60.000,0x0,2.000,transform,0") ;;
      "niri:msg output HDMI-A-1 position set -- 1920 180" | \
      "swaymsg:output HDMI-A-1 pos 1920 180" | \
      "hyprctl:keyword monitor HDMI-A-1,1280x800@60.000,1920x180,1.000,transform,0" | \
      "wlr-randr:--output DP-1 --pos 0,0 --output HDMI-A-1 --pos 1920,180" | \
      "kscreen-doctor:output.DP-1.position.0,0 output.HDMI-A-1.position.1920,180" | \
      "gdctl:set --logical-monitor --primary --x 0 --y 0 --scale 2 --transform normal --monitor DP-1 --mode 3840x2160@60.000 --logical-monitor --x 1920 --y 180 --scale 1 --transform normal --monitor HDMI-A-1 --mode 1280x800@60.000")
        touch "/tmp/applied-$DISPLAY_LAYOUT_TEST_BACKEND" ;;
      *)
        echo "unexpected compositor command: $command $*" >&2
        exit 1 ;;
    esac
    SH
    chmod +x $out/bin/compositor-fixture
    for name in niri swaymsg hyprctl wlr-randr kscreen-doctor gdctl; do
      ln -s compositor-fixture $out/bin/$name
    done
  '';
  swayConfig = pkgs.writeText "display-layout-test-sway.conf" ''
    output HEADLESS-1 mode 1280x800
    default_border none
    default_floating_border none
    for_window [app_id="display-layout-editor"] floating enable
  '';
in
pkgs.testers.runNixOSTest {
  name = "display-layout-backend-matrix";
  nodes.machine = {
    users.users.alice = {
      isNormalUser = true;
      extraGroups = [ "input" ];
    };
    environment.systemPackages = [
      package
      pkgs.grim
      pkgs.sway
      pkgs.wtype
    ];
    virtualisation.memorySize = 2048;
  };
  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")
    machine.succeed("install -d -m 0700 -o alice -g users /run/user/1000")
    machine.execute(
        "su -s ${pkgs.runtimeShell} alice -c '"
        "XDG_RUNTIME_DIR=/run/user/1000 "
        "WLR_BACKENDS=headless WLR_RENDERER=pixman "
        "${pkgs.sway}/bin/sway --unsupported-gpu --config ${swayConfig} "
        ">/tmp/sway.log 2>&1 &'"
    )
    machine.wait_until_succeeds(
        "find /run/user/1000 -maxdepth 1 -type s -name 'sway-ipc.*.sock' | grep -q .",
        timeout=20,
    )
    machine.wait_until_succeeds(
        "find /run/user/1000 -maxdepth 1 -type s -name 'wayland-*' | grep -q .",
        timeout=20,
    )
    wayland_display = machine.succeed(
        "basename $(find /run/user/1000 -maxdepth 1 -type s -name 'wayland-*' | head -1)"
    ).strip()
    sway_socket = machine.succeed(
        "find /run/user/1000 -maxdepth 1 -type s -name 'sway-ipc.*.sock' | head -1"
    ).strip()
    wayland_env = "XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=" + wayland_display
    sway_env = "SWAYSOCK=" + sway_socket

    for backend in ["niri", "sway", "hyprland", "wlr", "kscreen", "gnome"]:
        with subtest(f"render and apply through {backend}"):
            machine.execute(
                "su -s ${pkgs.runtimeShell} alice -c '" + wayland_env +
                " DISPLAY_LAYOUT_TEST_BACKEND=" + backend +
                " PATH=${fixtures}/bin:$PATH ${package}/bin/.display-layout-wrapped --backend " +
                backend + " >/tmp/display-layout-" + backend + ".log 2>&1 &'"
            )
            machine.wait_until_succeeds(
                sway_env + " ${pkgs.sway}/bin/swaymsg -t get_tree "
                "| grep -q '\"app_id\": \"display-layout-editor\"'",
                timeout=10,
            )
            # wl_pointer v5 emits a frame event after enter/motion. Keeping a
            # real libinput pointer on the seat makes null listener callbacks
            # fail here instead of only in interactive compositor sessions.
            machine.succeed(
                sway_env + " ${pkgs.sway}/bin/swaymsg "
                "seat seat0 cursor set 640 400"
            )
            machine.wait_until_succeeds(
                sway_env + " ${pkgs.sway}/bin/swaymsg -t get_tree "
                "| grep -q '\"app_id\": \"display-layout-editor\"'",
                timeout=5,
            )
            machine.succeed(
                "su -s ${pkgs.runtimeShell} alice -c '" + wayland_env +
                " ${pkgs.grim}/bin/grim /tmp/backend-" + backend + ".png'"
            )
            machine.copy_from_machine("/tmp/backend-" + backend + ".png")
            machine.succeed(
                "su -s ${pkgs.runtimeShell} alice -c '" + wayland_env +
                " ${pkgs.wtype}/bin/wtype -s 250 -k Return'"
            )
            machine.wait_until_succeeds("test -e /tmp/applied-" + backend, timeout=10)
            machine.wait_until_fails(
                sway_env + " ${pkgs.sway}/bin/swaymsg -t get_tree "
                "| grep -q '\"app_id\": \"display-layout-editor\"'"
            )
  '';
}
