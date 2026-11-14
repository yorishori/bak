# Build and install with:  makepkg -si
pkgname=bak
pkgver=r1.382cbea
pkgrel=1
pkgdesc="Simple TUI tool for verified, compressed (optionally encrypted) folder backups"
arch=('x86_64' 'aarch64' 'armv7h' 'i686')
url="https://github.com/yorishori/bak"
license=('unknown')
depends=('glibc' 'tar' 'xz')
optdepends=('gnupg: encrypted backups'
            'rclone: uploading backups')
makedepends=('git')
source=("git+https://github.com/yorishori/bak.git")
sha256sums=('SKIP')

pkgver() {
    cd "$pkgname"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$pkgname"
    make
}

package() {
    cd "$pkgname"
    make DESTDIR="$pkgdir" PREFIX=/usr install
}
