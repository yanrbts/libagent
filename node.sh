NODE_VER="v22.14.0"
ARCH="x64"
TARBALL="node-${NODE_VER}-linux-${ARCH}.tar.xz"

mkdir -p "$HOME/.local/src" "$HOME/.local/node22"
curl -fsSL "https://nodejs.org/dist/${NODE_VER}/${TARBALL}" -o "$HOME/.local/src/${TARBALL}"
tar -xJf "$HOME/.local/src/${TARBALL}" -C "$HOME/.local/node22" --strip-components=1

export PATH="$HOME/.local/node22/bin:$PATH"
node -v
