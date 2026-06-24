# Top-level (source) package metadata. The source tarball name is unchanged
# from earlier (fileengine-core-<ver>.tar.gz) so existing build scripts that
# pass --define name=fileengine-core still work.
%define _source_name fileengine-core
%define _libname     libfileengine_core.so

Name:           fileengine-core
Version:        1.0.0
Release:        1%{?dist}
Summary:        Distributed virtual filesystem — meta package
License:        MIT
URL:            https://github.com/fileengine/fileengine-core
Source0:        %{name}-%{version}.tar.gz
BuildRoot:      %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:  cmake, gcc-c++, make, pkgconfig
# Static libstdc++ archive for the self-contained fileengine-cli-static build.
BuildRequires:  libstdc++-static
BuildRequires:  postgresql-devel, openssl-devel, zlib-devel
BuildRequires:  grpc-devel, protobuf-devel, protobuf-compiler
# On modern Fedora the AWS SDK is split per service; the s3 client lives in
# aws-sdk-cpp-storage-devel and the core in aws-sdk-cpp-core-devel.
BuildRequires:  aws-sdk-cpp-core-devel
BuildRequires:  aws-sdk-cpp-storage-devel
BuildRequires:  libcurl-devel, libuuid-devel
BuildRequires:  systemd-rpm-macros
# Monitoring (Phase A): libsystemd for sd_notify; cpp-httplib + nlohmann/json
# are vendored under third_party/, no system packages needed for those.
BuildRequires:  systemd-devel
BuildRequires:  pkgconfig(libsystemd)

%description
FileEngine Core is a C++17 distributed virtual filesystem with multi-tenant
file management, POSIX ACLs, S3/MinIO object store integration, and a gRPC API.

This source package produces five binary packages:

* fileengine-libs         — shared library (libfileengine_core.so)
* fileengine-server       — gRPC server daemon + systemd unit + config
* fileengine-server-devel — headers + .so symlink for building integrations
* fileengine-cli          — command-line client (dynamic; needs fileengine-libs)
* fileengine-cli-static   — self-contained client (no fileengine-libs / libstdc++)

The empty fileengine-core meta-package depends on -libs and -server for the
common "install everything for a server box" case. On a host that already runs
the full stack install fileengine-cli; on a client-only machine install
fileengine-cli-static for a leaner footprint.

Requires:       fileengine-libs = %{version}-%{release}
Requires:       fileengine-server = %{version}-%{release}

# ---------------------------------------------------------------------------
# Shared library
# ---------------------------------------------------------------------------
%package -n fileengine-libs
Summary:        FileEngine shared library
Requires:       postgresql, openssl, libcurl, libuuid
%description -n fileengine-libs
The libfileengine_core.so shared library used by the FileEngine server and
client binaries. Install on any machine that runs either of them.

%post -n fileengine-libs -p /sbin/ldconfig
%postun -n fileengine-libs -p /sbin/ldconfig

%files -n fileengine-libs
%defattr(-,root,root,-)
%attr(644,root,root) %{_libdir}/%{_libname}

# ---------------------------------------------------------------------------
# Server daemon
# ---------------------------------------------------------------------------
%package -n fileengine-server
Summary:        FileEngine gRPC server daemon
Requires:       fileengine-libs = %{version}-%{release}
Requires(pre):  shadow-utils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
%description -n fileengine-server
The fileengine_server gRPC daemon. Multi-tenant virtual filesystem with
PostgreSQL persistence, S3/MinIO object-store integration, ACL/RBAC
enforcement, and at-rest compression+encryption.

Ships a systemd unit (fileengine.service), a default config at
/etc/fileengine/core.conf, and a logrotate rule. Creates a system 'fileengine'
user/group and the storage and log directories on install.

%pre -n fileengine-server
# Create fileengine user/group before file install so ownership lands right.
getent group fileengine >/dev/null || groupadd -r fileengine
getent passwd fileengine >/dev/null || \
    useradd -r -g fileengine -d /var/lib/fileengine -s /sbin/nologin \
            -c "FileEngine Service" fileengine
exit 0

%post -n fileengine-server
# Storage + log directories owned by the service user.
install -d -o fileengine -g fileengine -m 750 /var/lib/fileengine/storage
install -d -o fileengine -g fileengine -m 750 /var/log/fileengine
%systemd_post fileengine.service

%preun -n fileengine-server
%systemd_preun fileengine.service

%postun -n fileengine-server
%systemd_postun_with_restart fileengine.service

%files -n fileengine-server
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/fileengine_server
%config(noreplace) /etc/fileengine/core.conf
%config(noreplace) /etc/logrotate.d/fileengine
%{_unitdir}/fileengine.service
%dir %attr(750,fileengine,fileengine) /var/lib/fileengine
%dir %attr(750,fileengine,fileengine) /var/log/fileengine

# ---------------------------------------------------------------------------
# Development headers (build integrations against the shared library)
# ---------------------------------------------------------------------------
%package -n fileengine-server-devel
Summary:        Development headers for building against fileengine-libs
Requires:       fileengine-libs = %{version}-%{release}
%description -n fileengine-server-devel
C++ headers and a libfileengine_core.so dev symlink for linking other
fileengine components against the shared library. Install on dev hosts.

%files -n fileengine-server-devel
%defattr(-,root,root,-)
%{_includedir}/fileengine/

# ---------------------------------------------------------------------------
# CLI client
# ---------------------------------------------------------------------------
%package -n fileengine-cli
Summary:        Command-line client for the FileEngine server
Requires:       fileengine-libs = %{version}-%{release}
%description -n fileengine-cli
The fileengine_cli command-line tool. Connects to a FileEngine gRPC server
(local or remote) for directory/file/permission/version/metadata operations.

%files -n fileengine-cli
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/fileengine_cli

# ---------------------------------------------------------------------------
# CLI client — self-contained (statically-linked) build
# ---------------------------------------------------------------------------
%package -n fileengine-cli-static
Summary:        Self-contained command-line client for the FileEngine server
%description -n fileengine-cli-static
A self-contained build of the FileEngine command-line client
(fileengine_cli_static). It does NOT depend on fileengine-libs or a matching
libstdc++ — both are linked in — so it can be dropped onto a client machine to
talk to a remote FileEngine server without installing the rest of the stack.

The heavy third-party libraries (gRPC, protobuf, OpenSSL, libpq) are still
linked dynamically and pulled in via the usual shared-library dependencies.

Install fileengine-cli instead on hosts that already run the full FileEngine
stack; install this package for a leaner, client-only footprint.

%files -n fileengine-cli-static
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/fileengine_cli_static

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
%prep
%setup -q -n %{_source_name}-%{version}

%build
# Plain cmake invocation (avoid the cmake distro macro): the upstream
# CMakeLists is structured around an in-tree build/ subdir; Fedora's
# distro macro redirects into redhat-linux-build/ which leaves build/
# empty for the install phase below.
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=%{_lib} \
    -DCMAKE_SKIP_INSTALL_RPATH=ON \
    -DBUILD_STATIC_CLI=ON
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
cd build
make DESTDIR=%{buildroot} install

# Drop the CMake-driven /usr/local symlinks and /etc/systemd/system unit;
# the spec installs to %{_unitdir} explicitly below.
rm -rf %{buildroot}/usr/local
rm -rf %{buildroot}/etc/systemd

# Sytemd unit (canonical RPM location).
install -d %{buildroot}%{_unitdir}
install -m 644 ../fileengine.service %{buildroot}%{_unitdir}/fileengine.service

# Default server config.
install -d %{buildroot}/etc/fileengine
install -m 644 ../core.conf %{buildroot}/etc/fileengine/core.conf

# Logrotate.
install -d %{buildroot}/etc/logrotate.d
install -m 644 ../fileengine.logrotate %{buildroot}/etc/logrotate.d/fileengine

# Pre-create runtime dirs so the %dir directives have something to own.
install -d %{buildroot}/var/lib/fileengine
install -d %{buildroot}/var/log/fileengine

%clean
rm -rf %{buildroot}

%changelog
* Fri Jun 19 2026 FileEngine Team <maintainer@fileengine.example.com> - 1.0.0-1
- Add fileengine-cli-static: a self-contained statically-linked CLI package
  (no fileengine-libs / libstdc++ dependency) for client-only machines,
  alongside the existing dynamic fileengine-cli. Build now passes
  -DBUILD_STATIC_CLI=ON and BuildRequires libstdc++-static.

* Wed Jun 17 2026 FileEngine Team <maintainer@fileengine.example.com> - 1.0.0-1
- Split monolithic fileengine-core RPM into four sub-packages:
  fileengine-libs, fileengine-server, fileengine-server-devel, fileengine-cli.
- Library now triggers ldconfig in %post; server runs as system user
  'fileengine'; per-package %files lists keep contents disjoint.

* Wed Jan 07 2026 FileEngine Team <maintainer@fileengine.example.com> - 1.0.0-1
- Initial package
