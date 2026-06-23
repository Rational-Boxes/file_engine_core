# Single-stage build on Fedora (matches the project's known-good build env).
# AWS SDK is optional (find_package(AWSSDK QUIET)); omitted here, so S3 uses the
# vendored httplib client. Multi-stage slimming is a later refinement.
FROM fedora:43

RUN dnf -y install \
        gcc-c++ cmake make pkgconf-pkg-config git which \
        grpc-devel grpc-plugins protobuf-devel protobuf-compiler \
        libpq-devel zlib-devel openssl-devel libuuid-devel libcurl-devel \
        systemd-devel \
    && dnf clean all

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target fileengine_server -j"$(nproc)" \
    && install -Dm755 build/core/fileengine_server /usr/local/bin/fileengine_server

WORKDIR /app
EXPOSE 50051
# Config (DB/S3/LDAP/keys) is supplied via environment variables at run time.
ENTRYPOINT ["/usr/local/bin/fileengine_server"]
