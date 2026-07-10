{ inputs, self, ... }:
{
  imports = [
    inputs.git-hooks.flakeModule
    inputs.treefmt-nix.flakeModule
  ];

  perSystem =
    {
      config,
      pkgs,
      system,
      ...
    }:
    {
      treefmt = {
        projectRootFile = "flake.nix";
        # Vendored headers must remain byte-for-byte identical to their pinned
        # upstream revisions; the integrity check below enforces this.
        settings.global.excludes = [ "src/third_party/**" ];
        programs = {
          clang-format.enable = true;
          nixfmt.enable = true;
          prettier.enable = true;
        };
      };

      pre-commit.settings.hooks = {
        treefmt.enable = true;
        deadnix.enable = true;
        markdownlint.enable = true;
        statix.enable = true;
        desktop-file-validate = {
          enable = true;
          name = "desktop file validation";
          entry = "${pkgs.desktop-file-utils}/bin/desktop-file-validate";
          files = "\\.desktop$";
        };
        tests = {
          enable = true;
          name = "C tests";
          entry = toString (
            pkgs.writeShellScript "display-layout-tests" ''
              ${pkgs.gnumake}/bin/make CC=${pkgs.stdenv.cc}/bin/cc check
            ''
          );
          files = "\\.(c|h)$";
          pass_filenames = false;
        };
      };

      checks = {
        package = self.packages.${system}.default;
        formatting = config.treefmt.build.check self;
        third-party-integrity = pkgs.runCommand "third-party-integrity" { } ''
          echo 'c04533e9181e1e33baceb0f55ac449b05145bb936e8c68cc77dfe0d8277514fb  ${self}/src/third_party/jsmn.h' | sha256sum --check
          echo 'ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab  ${self}/src/third_party/stb_truetype.h' | sha256sum --check
          touch $out
        '';
      };

      devShells.default = pkgs.mkShell {
        packages = [
          pkgs.clang-tools
          pkgs.gdb
          pkgs.gnumake
          pkgs.desktop-file-utils
          pkgs.libX11
          pkgs.libXrender
        ];
        shellHook = config.pre-commit.installationScript;
      };
    };
}
