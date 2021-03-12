#
# Copyright(c) 2012-2021 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause-Clear
#

CASCTL_DIR = /lib/opencas
UDEVRULES_DIR = /lib/udev/rules.d
UDEV:=$(shell which udevadm)
SYSTEMCTL := $(shell which systemctl)
PYTHON3 := $(shell which python3)

ifneq "$(wildcard /usr/lib/systemd/system)" ""
	SYSTEMD_DIR=/usr/lib/systemd/system
else
	SYSTEMD_DIR=/lib/systemd/system
endif

# Just a placeholder when running make from parent dir without install/uninstall arg
all: ;

install: install_files
ifeq (, $(PYTHON3))
	$(error package 'python3' not found)
else
	@$(UDEV) control --reload-rules
	@$(SYSTEMCTL) daemon-reload
	@$(SYSTEMCTL) -q enable open-cas.target

install_files:
	@echo "Installing Open-CAS utils"

	@install -m 755 -d $(DESTDIR)$(CASCTL_DIR)
	@install -m 644 opencas.py $(DESTDIR)$(CASCTL_DIR)/opencas.py
	@install -m 644 cas-service.template $(DESTDIR)$(CASCTL_DIR)/cas-service.template
	@install -m 755 casctl $(DESTDIR)$(CASCTL_DIR)/casctl
	@install -m 755 cas-service $(DESTDIR)$(CASCTL_DIR)/cas-service
	@install -m 755 open-cas-loader $(DESTDIR)$(CASCTL_DIR)/open-cas-loader

	@mkdir -p $(DESTDIR)/etc/dracut.conf.d/
	@install -m 644 etc/dracut.conf.d/opencas.conf $(DESTDIR)/etc/dracut.conf.d/opencas.conf

	@mkdir -p $(DESTDIR)/sbin
	@ln -fs $(CASCTL_DIR)/casctl $(DESTDIR)/sbin/casctl
	@ln -fs $(CASCTL_DIR)/cas-service $(DESTDIR)/sbin/cas-service

	@mkdir -p $(DESTDIR)$(UDEVRULES_DIR)
	@install -m 644 60-persistent-storage-cas-load.rules $(DESTDIR)$(UDEVRULES_DIR)/60-persistent-storage-cas-load.rules
	@install -m 644 60-persistent-storage-cas.rules $(DESTDIR)$(UDEVRULES_DIR)/60-persistent-storage-cas.rules

	@install -m 755 -d $(DESTDIR)/usr/share/doc/opencas

	@mkdir -p $(DESTDIR)/usr/share/man/man8
	@install -m 644 casctl.8 $(DESTDIR)/usr/share/man/man8/casctl.8

	@mkdir -p $(DESTDIR)$(SYSTEMD_DIR)
	@install -m 644 open-cas.target $(DESTDIR)$(SYSTEMD_DIR)/open-cas.target
	@install -m 755 -d $(DESTDIR)$(SYSTEMD_DIR)/../system-shutdown
	@install -m 755 open-cas.shutdown $(DESTDIR)$(SYSTEMD_DIR)/../system-shutdown/open-cas.shutdown
	@install -m 755 open-cas-generator $(DESTDIR)$(SYSTEMD_DIR)/../system-generators/open-cas-generator
endif

uninstall:
	@rm $(DESTDIR)$(CASCTL_DIR)/opencas.py
	@rm $(DESTDIR)$(CASCTL_DIR)/casctl
	@rm $(DESTDIR)$(CASCTL_DIR)/open-cas-loader
	@rm $(DESTDIR)$(CASCTL_DIR)/cas-service.template
	@rm -rf $(DESTDIR)$(CASCTL_DIR)

	@rm $(DESTDIR)/etc/dracut.conf.d/opencas.conf

	@rm $(DESTDIR)/sbin/casctl
	@rm $(DESTDIR)/sbin/cas-service

	@rm $(DESTDIR)/usr/share/man/man8/casctl.8

	@rm $(DESTDIR)$(UDEVRULES_DIR)/60-persistent-storage-cas-load.rules
	@rm $(DESTDIR)$(UDEVRULES_DIR)/60-persistent-storage-cas.rules
	@$(UDEV) control --reload-rules

	@rm $(DESTDIR)$(SYSTEMD_DIR)/open-cas.target
	@rm $(DESTDIR)$(SYSTEMD_DIR)/../system-shutdown/open-cas.shutdown
	@rm $(DESTDIR)$(SYSTEMD_DIR)/../system-generators/open-cas-generator

	@$(SYSTEMCTL) daemon-reload

.PHONY: install uninstall clean distclean
