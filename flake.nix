{
  description = "OpenElevator - configurable elevator simulator";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";

    # arduino-nix wraps arduino-cli with cores and libraries as Nix
    # derivations. No runtime downloads. No _ARDUINO_PYTHON3 workaround.
    # https://github.com/bouk/arduino-nix
    arduino-nix = {
      url   = "github:bouk/arduino-nix";
    };

    # Official Arduino package index. wrapArduinoCLI unconditionally
    # requires pkgs.arduinoPackages.tools.builtin, which lives here.
    # Update with: nix flake update arduino-index
    arduino-index = {
      url   = "https://downloads.arduino.cc/packages/package_index.json";
      flake = false;
    };

    # PJRC Teensy package index.
    # Update with: nix flake update teensy-index
    teensy-index = {
      url   = "https://www.pjrc.com/teensy/package_teensy_index.json";
      flake = false;
    };

    # Arduino library index.
    # Update with: nix flake update library-index
    library-index = {
      url   = "https://downloads.arduino.cc/libraries/library_index.json";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      arduino-nix,
      arduino-index,
      teensy-index,
      library-index,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [
            # Exposes pkgs.wrapArduinoCLI
            arduino-nix.overlay
            # Populates pkgs.arduinoPackages.tools.builtin (required by wrapArduinoCLI)
            (arduino-nix.mkArduinoPackageOverlay arduino-index)
            # Populates pkgs.arduinoPackages.platforms.teensy and tools.teensy
            (arduino-nix.mkArduinoPackageOverlay teensy-index)
            # Populates pkgs.arduinoLibraries
            (arduino-nix.mkArduinoLibraryOverlay library-index)
            # Teensy packages >=1.59.0 ship as .tar.zst. arduino-nix only
            # adds unzip to nativeBuildInputs so stdenv's unpackPhase cannot
            # handle them. This overlay patches both the platform derivations
            # (teensy-avr-x.y.z) and the tool derivations (teensy-tools-x.y.z)
            # under the teensy namespace to include zstd.
            (final: prev:
              let
                addZstd = drv: drv.overrideAttrs (old: {
                  nativeBuildInputs =
                    (old.nativeBuildInputs or [ ]) ++ [ final.zstd ];
                });
                patchVersions  = versions: builtins.mapAttrs (_: addZstd) versions;
                patchNamespace = ns: builtins.mapAttrs (_: patchVersions) ns;
              in {
                arduinoPackages = prev.lib.recursiveUpdate prev.arduinoPackages {
                  platforms.teensy = patchNamespace prev.arduinoPackages.platforms.teensy;
                  tools.teensy     = patchNamespace prev.arduinoPackages.tools.teensy;
                };
              })
          ];
        };

        # latestVersion picks the highest semver from whatever the pinned
        # index contains. To pin to a specific version instead, replace
        # latestVersion with the version string, e.g.:
        #   pkgs.arduinoPackages.platforms.teensy.avr."1.59.0"
        # Browse available versions from the nix repl:
        #   nix repl> :lf .
        #   nix repl> pkgs.arduinoPackages.platforms.teensy.avr
        #   nix repl> pkgs.arduinoLibraries.U8g2
        inherit (arduino-nix) latestVersion;

        arduinoCLI = pkgs.wrapArduinoCLI {
          packages = [
            (latestVersion pkgs.arduinoPackages.platforms.teensy.avr)
          ];
          libraries = [
            (latestVersion pkgs.arduinoLibraries."U8g2")
          ];
        };

      in
      {
        devShells.default = pkgs.mkShell {
          name = "open-elevator-dev";
          packages = [
            arduinoCLI
            pkgs.gnumake
            pkgs.picocom
            pkgs.teensy-loader-cli
            pkgs.git
          ];
          shellHook = ''
            >&2 echo "==> arduino-cli $(arduino-cli version)"
            >&2 echo "    Board: Teensy 4.1 (teensy:avr:teensy41)"
          '';
        };

        formatter = pkgs.nixfmt-tree;
      }
    );
}
