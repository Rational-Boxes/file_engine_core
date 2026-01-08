# Maintainer: FileEngine Team
pkgname=fileengine-core
pkgver=1.0.0
pkgrel=1
pkgdesc="A distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment support"
arch=('x86_64')
url="https://github.com/fileengine/fileengine-core"
license=('MIT')
depends=('postgresql' 'openssl' 'zlib' 'aws-sdk-cpp' 'grpc' 'protobuf' 'curl' 'libuuid')
makedepends=('cmake' 'gcc' 'make')

source=("fileengine-core-${pkgver}.tar.gz::https://github.com/fileengine/fileengine-core/archive/v${pkgver}.tar.gz")
sha256sums=('SKIP')  # Replace with actual checksum when available

build() {
    cd "${srcdir}/fileengine-core-${pkgver}"
    
    mkdir -p build
    cd build
    
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib
    make
}

package() {
    cd "${srcdir}/fileengine-core-${pkgver}/build"
    
    make DESTDIR="${pkgdir}" install
    
    # Install systemd service file
    install -Dm644 "${srcdir}/fileengine.service" "${pkgdir}/usr/lib/systemd/system/fileengine.service"
    
    # Install default config
    install -Dm644 "${srcdir}/fileengine.conf" "${pkgdir}/etc/fileengine/core.conf"

    # Install logrotate configuration
    install -Dm644 "${srcdir}/fileengine.logrotate" "${pkgdir}/etc/logrotate.d/fileengine"

    # Install documentation
    install -Dm644 "${srcdir}/README.md" "${pkgdir}/usr/share/doc/fileengine/README.md"
    install -Dm644 "${srcdir}/LICENSE" "${pkgdir}/usr/share/licenses/fileengine/LICENSE"
}

# vim:set ts=2 sw=2 et: