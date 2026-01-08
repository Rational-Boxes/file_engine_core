Name:           fileengine-core
Version:        1.0.0
Release:        1%{?dist}
Summary:        A distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment support

License:        MIT
Group:          System Environment/Daemons
URL:            https://github.com/fileengine/fileengine-core
Source0:        %{name}-%{version}.tar.gz
BuildRoot:      %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:  cmake, gcc-c++, make
BuildRequires:  postgresql-devel, openssl-devel, zlib-devel
BuildRequires:  grpc-devel, protobuf-devel, protobuf-compiler
BuildRequires:  aws-sdk-cpp-devel, libcurl-devel, libuuid-devel

Requires:       postgresql, openssl, libcurl, libuuid

%description
FileEngine Core is a simplified, focused implementation of a distributed
virtual filesystem with horizontal scaling and hybrid cloud/on-premises
deployment support. It provides features like:

* UUID-based file identification for distributed handling
* Automatic versioning with microsecond precision timestamps
* POSIX-compliant ACLs for granular access control
* Intelligent file culling with configurable thresholds
* Hybrid cloud/on-premises deployment support
* S3/MinIO integration with automatic synchronization
* Detailed storage tracking per host and tenant

%prep
%setup -q

%build
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=lib
make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT
cd build
make DESTDIR=$RPM_BUILD_ROOT install

# Install systemd service file
mkdir -p $RPM_BUILD_ROOT%{_unitdir}
install -m 644 ../fileengine.service $RPM_BUILD_ROOT%{_unitdir}/fileengine.service

# Install default configuration
mkdir -p $RPM_BUILD_ROOT/etc/fileengine
install -m 644 ../core.conf $RPM_BUILD_ROOT/etc/fileengine/core.conf

%post
# Create fileengine user if it doesn't exist
getent group fileengine >/dev/null || groupadd -r fileengine
getent passwd fileengine >/dev/null || useradd -r -g fileengine -d /var/lib/fileengine -s /sbin/nologin -c "FileEngine Core Service" fileengine
# Create storage directory
mkdir -p /var/lib/fileengine/storage
chown -R fileengine:fileengine /var/lib/fileengine
# Enable and start the service
%systemd_post fileengine.service

%preun
%systemd_preun fileengine.service

%postun
%systemd_postun_with_restart fileengine.service

%files
%attr(755,root,root) %{_bindir}/fileengine_server
%attr(755,root,root) %{_bindir}/fileengine_cli
%attr(644,root,root) %{_libdir}/libfileengine_core.so
%attr(644,root,root) %{_datadir}/fileengine/
%config(noreplace) /etc/fileengine/core.conf
%{_unitdir}/fileengine.service
%dir /var/lib/fileengine
%dir /var/log/fileengine

%changelog
* Wed Jan 07 2026 FileEngine Team <maintainer@fileengine.example.com> - 1.0.0-1
- Initial package