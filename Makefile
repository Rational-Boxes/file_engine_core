# FileEngine Core Packaging Makefile

.PHONY: dist arch-package deb-package rpm-package clean

# Variables
VERSION := 1.0.0
PACKAGE_NAME := fileengine-core
BUILD_DIR := build
INSTALL_PREFIX := /opt/file_engine_core

# Create distribution tarball
dist:
	@echo "Creating distribution tarball..."
	@git archive --format=tar.gz --output=$(PACKAGE_NAME)-$(VERSION).tar.gz HEAD

# Build Arch Linux package
arch-package: dist
	@echo "Building Arch Linux package..."
	@cp $(PACKAGE_NAME)-$(VERSION).tar.gz PKGBUILD ./
	@updpkgsums
	@makepkg -si

# Build Debian package
deb-package: dist
	@echo "Building Debian package..."
	@mkdir -p $(BUILD_DIR)/debian-build
	@cp -r debian $(BUILD_DIR)/debian-build/
	@cp $(PACKAGE_NAME)-$(VERSION).tar.gz $(BUILD_DIR)/
	@cd $(BUILD_DIR) && tar -xzf $(PACKAGE_NAME)-$(VERSION).tar.gz
	@cd $(BUILD_DIR)/fileengine-core-$(VERSION) && dpkg-buildpackage -us -uc -b

# Build RPM package
rpm-package: dist
	@echo "Building RPM package..."
	@mkdir -p ~/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SRPMS,SPECS}
	@cp $(PACKAGE_NAME)-$(VERSION).tar.gz ~/rpmbuild/SOURCES/
	@cp fileengine-core.spec ~/rpmbuild/SPECS/
	@rpmbuild -bb ~/rpmbuild/SPECS/fileengine-core.spec

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f *.tar.gz
	@rm -f *.deb
	@rm -f *.rpm

# Install the project
install:
	@echo "Installing FileEngine Core..."
	@cd $(BUILD_DIR) && cmake .. -DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) && make && make install

# Uninstall the project
uninstall:
	@echo "Uninstalling FileEngine Core..."
	@rm -rf $(INSTALL_PREFIX)
	@rm -f /usr/local/bin/fileengine_server
	@rm -f /usr/local/bin/fileengine_cli
	@rm -f /usr/local/lib/libfileengine_core.so
	@rm -f /usr/local/include/fileengine