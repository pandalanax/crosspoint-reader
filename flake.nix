{
  description = "CrossPoint Reader – ESP32-C3 e-reader firmware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Python with no extra deps — all build scripts use stdlib only
        python = pkgs.python3;

        # PlatformIO needs to download toolchains into ~/.platformio.
        # For `nix build` we use an FHS sandbox so it can do this in an
        # isolated environment.  For `nix develop` we just put pio on PATH
        # and let it use the real ~/.platformio.
        pioCoreEnv = pkgs.platformio-core;

        # Common native build inputs needed by PlatformIO's ESP32 toolchain
        buildInputs = [
          pioCoreEnv
          python
          pkgs.git
          pkgs.cacert
          pkgs.esptool
        ];

        # FHS env that satisfies PlatformIO's expectations for `nix build`
        fhsEnv = pkgs.buildFHSEnv {
          name = "crosspoint-fhs";
          targetPkgs =
            _:
            buildInputs
            ++ [
              pkgs.stdenv.cc.cc.lib
              pkgs.zlib
              pkgs.libusb1
            ];
        };
      in
      {
        # ── nix develop ──────────────────────────────────────────────
        devShells.default = pkgs.mkShell {
          packages = buildInputs ++ [
            pkgs.clang-tools # clang-format, clang-tidy
            pkgs.minicom # serial monitor
            pkgs.usbutils # lsusb
          ];

          shellHook = ''
            echo "CrossPoint Reader dev shell"
            echo "  pio run              – build firmware (default env)"
            echo "  pio run -t upload    – flash to device"
            echo "  pio run -e gh_release – production build"
            echo ""
            echo "First run will download the ESP32-C3 toolchain to ~/.platformio/"

            # Ensure submodule is present
            if [ ! -f open-x4-sdk/README.md ]; then
              echo "Initializing git submodules..."
              git submodule update --init --recursive
            fi
          '';

          # PlatformIO shells out to git for version detection
          GIT_DISCOVERY_ACROSS_FILESYSTEM = "1";
        };

        # ── nix build ────────────────────────────────────────────────
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "crosspoint-reader";
          version = "1.2.0";

          src = self;

          nativeBuildInputs = [
            fhsEnv
            pkgs.git
            pkgs.cacert
          ];

          # PlatformIO needs HOME and network during build to fetch
          # the ESP32 toolchain + libraries on first run.
          # For a fully offline build this would need a fixed-output
          # derivation for ~/.platformio — see comment below.
          #
          # This derivation works with `--option sandbox false` or
          # with a pre-populated PLATFORMIO_CORE_DIR.
          __noChroot = true;

          buildPhase = ''
            export HOME=$(mktemp -d)
            export PLATFORMIO_CORE_DIR=$HOME/.platformio
            export GIT_DISCOVERY_ACROSS_FILESYSTEM=1
            export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt

            # Init submodules if needed
            if [ ! -f open-x4-sdk/README.md ]; then
              git submodule update --init --recursive
            fi

            crosspoint-fhs -c "pio run -e gh_release"
          '';

          installPhase = ''
            mkdir -p $out/firmware
            cp .pio/build/gh_release/firmware.bin $out/firmware/
            cp .pio/build/gh_release/partitions.bin $out/firmware/ 2>/dev/null || true
            cp .pio/build/gh_release/bootloader.bin $out/firmware/ 2>/dev/null || true

            # Also provide a flash script
            cat > $out/firmware/flash.sh <<'FLASH'
            #!/usr/bin/env bash
            set -euo pipefail
            PORT=''${1:-/dev/ttyACM0}
            DIR="$(cd "$(dirname "$0")" && pwd)"
            echo "Flashing CrossPoint Reader to $PORT..."
            esptool.py --chip esp32c3 --port "$PORT" --baud 921600 \
              --before default_reset --after hard_reset write_flash \
              0x0000 "$DIR/bootloader.bin" \
              0x8000 "$DIR/partitions.bin" \
              0x10000 "$DIR/firmware.bin"
            FLASH
            chmod +x $out/firmware/flash.sh
          '';

          meta = {
            description = "CrossPoint Reader firmware for Xteink X4";
            platforms = pkgs.lib.platforms.all;
          };
        };

        # ── nix run ──────────────────────────────────────────────────
        # Quick shortcut: `nix run .#flash -- /dev/ttyACM0`
        apps.flash = {
          type = "app";
          program = "${self.packages.${system}.default}/firmware/flash.sh";
        };
      }
    );
}
