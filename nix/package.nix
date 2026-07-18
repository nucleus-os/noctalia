{
  lib,
  config,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  freetype,
  fontconfig,
  libxkbcommon,
  sdbus-cpp_2,
  systemd,
  pipewire,
  pam,
  curl,
  libwebp,
  glib,
  polkit,
  libqalculate,
  libxml2,
  md4c,
  stb,
  fetchFromGitHub,
  nlohmann_json,
  tomlplusplus,
  wireplumber,
  makeWrapper,
  git,
  autoAddDriverRunpath,
  cudaSupport ? config.cudaSupport,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*version: '([^']+)'.*" (readFile ../meson.build));
  stb' = stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
in
stdenv.mkDerivation {
  pname = "noctalia";
  inherit version;

  src = lib.cleanSource ./..;

  postPatch = ''
    # Remove -march=native and -mtune=native for reproducible builds
    sed -i "s/'-march=native', '-mtune=native',//" meson.build
  '';

  postFixup = ''
    wrapProgram $out/bin/noctalia \
      --prefix PATH : ${lib.makeBinPath [ git ]}
  '';

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    makeWrapper
  ]
  ++ lib.optional cudaSupport autoAddDriverRunpath;

  buildInputs = [
    wayland
    wayland-protocols
    freetype
    fontconfig
    libxkbcommon
    sdbus-cpp_2
    systemd
    pipewire
    wireplumber
    pam
    curl
    libwebp
    glib
    polkit
    libqalculate
    libxml2
    md4c
    stb'
    nlohmann_json
    tomlplusplus
  ];

  mesonBuildType = "release";

  ninjaFlags = [ "-v" ];

  meta = with lib; {
    description = "A lightweight Wayland shell and bar built directly on Wayland, Vulkan, and Skia Graphite";
    homepage = "https://github.com/noctalia-dev/noctalia";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "noctalia";
  };
}
