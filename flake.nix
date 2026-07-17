{
  description = "A focused, backend-extensible display layout editor";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts = {
      url = "github:hercules-ci/flake-parts";
      inputs.nixpkgs-lib.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      flake-parts,
      ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      imports = [ inputs.flake-parts.flakeModules.partitions ];

      partitionedAttrs = {
        checks = "dev";
        devShells = "dev";
        formatter = "dev";
      };

      partitions.dev = {
        extraInputsFlake = ./dev;
        module = import ./nix/flake/dev-partition.nix;
      };

      perSystem =
        { system, ... }:
        let
          pkgs = import nixpkgs { inherit system; };
          version = self.shortRev or self.dirtyShortRev or "development";
          package = pkgs.stdenv.mkDerivation {
            pname = "display-layout";
            inherit version;
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./Makefile
                ./assets
                ./config.example.ini
                ./display-layout-editor.desktop
                ./src
                ./tests
              ];
            };

            strictDeps = true;
            nativeBuildInputs = [
              pkgs.makeWrapper
              pkgs.pkg-config
              pkgs.wayland-scanner
              pkgs.wayland-protocols
              pkgs.wlr-protocols
            ];
            buildInputs = [
              pkgs.libxkbcommon
              pkgs.wayland
            ];
            makeFlags = [
              "VERSION=${version}"
              "WLR_PROTOCOLS_DIR=${pkgs.wlr-protocols}/share/wlr-protocols"
              "WAYLAND_PROTOCOLS_DIR=${pkgs.wayland-protocols}/share/wayland-protocols"
            ];
            doCheck = true;
            checkTarget = "check";
            installFlags = [
              "DESTDIR=$(out)"
              "PREFIX="
            ];
            postFixup = ''
              wrapProgram $out/bin/display-layout \
                --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.wlr-randr ]}
            '';

            meta = {
              description = "Focused drag-and-drop display layout editor";
              homepage = "https://github.com/johnrichardrinehart/display-layout";
              license = pkgs.lib.licenses.mit;
              mainProgram = "display-layout";
              platforms = pkgs.lib.platforms.linux;
            };
          };
        in
        {
          packages = {
            default = package;
            display-layout = package;
          };
          apps.default = {
            type = "app";
            program = "${package}/bin/display-layout";
            meta.description = "Open the Display Layout Editor";
          };
          legacyPackages = pkgs;
        };
    };
}
