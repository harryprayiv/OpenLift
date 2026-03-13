{
  description = "OpenElevator - configurable elevator simulator";

  inputs = {
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs.url = "github:NixOS/nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "x86_64-darwin" "aarch64-linux" ] (
      system:
      let
        name = "open-elevator";

        pkgs = import nixpkgs { inherit system; };

        python = pkgs.python3;

        pythonWithExtras = python.buildEnv.override { extraLibs = [ ]; };

        # arduino-cli stores all downloaded cores, libraries, and staging
        # artifacts under a single project-scoped directory so that multiple
        # Arduino projects on the same machine do not stomp on each other.
        # The Teensy core comes from PJRC's own package index, not the
        # official Arduino index, so the first `make install` will pull from
        # both sources.
        #
        # Teensy upload on Linux requires udev rules for the PJRC USB IDs.
        # If uploads fail with a permissions error, install the rules from:
        # https://www.pjrc.com/teensy/00-teensy.rules
        # Copy to /etc/udev/rules.d/ and run: udevadm control --reload-rules

        arduinoShellHookPaths = ''
          if [ -z "''${_ARDUINO_PROJECT_DIR:-}" ]; then
            if [ -n "''${_ARDUINO_ROOT_DIR:-}" ]; then
              export _ARDUINO_PROJECT_DIR="''${_ARDUINO_ROOT_DIR}/${name}"
            elif [ -n "''${XDG_CACHE_HOME:-}" ]; then
              export _ARDUINO_PROJECT_DIR="''${XDG_CACHE_HOME}/arduino/${name}"
            else
              export _ARDUINO_PROJECT_DIR="''${HOME}/.arduino/${name}"
            fi
          fi

          export ARDUINO_DIRECTORIES_USER=$_ARDUINO_PROJECT_DIR
          export ARDUINO_DIRECTORIES_DATA=$_ARDUINO_PROJECT_DIR
          export ARDUINO_DIRECTORIES_DOWNLOADS=$_ARDUINO_PROJECT_DIR/staging

          export _ARDUINO_PYTHON3=${python}
        '';

        devShellArduinoCLI = pkgs.mkShell {
          name = "${name}-dev";
          packages = with pkgs; [
            arduino-cli
            git
            gnumake
            picocom
            teensy-loader-cli
            pythonWithExtras
          ];
          shellHook = ''
            ${arduinoShellHookPaths}
            >&2 echo "==> Using arduino-cli version $(arduino-cli version)"
            >&2 echo "    Storing arduino-cli data for this project in '$_ARDUINO_PROJECT_DIR'"
            >&2 echo "    Board: Teensy 4.1 (teensy:avr:teensy41)"
            >&2 echo "    Run 'make install' to fetch the Teensy core and libraries."
          '';
        };

      in
      {
        devShells = {
          inherit devShellArduinoCLI;
        };

        devShells.default = devShellArduinoCLI;

        formatter = pkgs.nixfmt-tree;
      }
    );
}
