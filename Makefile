PYTHON ?= python3
ARM_IMAGE ?= mpc-usb-plugin-installer-build-tools

OFFLINE_ARGS ?=
.DEFAULT_GOAL := arm
-include private/offline-tests.mk

.PHONY: arm arm-image test test-local test-runner test-offline syntax site

arm: arm-image
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh build.sh

arm-image:
	docker build --platform linux/arm/v7 -t $(ARM_IMAGE) .

test: arm
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh -c 'build/shortcut-state-test && MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build LD_PRELOAD=/work/build/shortcut-hook.so build/shortcut-hook-test'
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh -c 'MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build LD_PRELOAD="/work/build/shortcut-addin-first.so /work/build/shortcut-addin-second.so /work/build/shortcut-hook.so" build/shortcut-hook-test --chained'

test-runner:
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" LEGACY_PACKAGE_ZIP="$(LEGACY_PACKAGE_ZIP)" $(PYTHON) tests/offline_runner_test.py

test-local:
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" LEGACY_PACKAGE_ZIP="$(LEGACY_PACKAGE_ZIP)" $(PYTHON) -m unittest discover -s tests -p offline_runner_test.py
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" $(PYTHON) -m unittest discover -s tests -p package_test.py
	$(PYTHON) -m unittest discover -s tests -p firmware_test.py
	$(PYTHON) -m unittest discover -s tests -p saltscan_test.py
	$(PYTHON) -m unittest discover -s tests -p setup_test.py

site:
	$(PYTHON) tools/build_site.py

test-offline:
	@test -n "$(FIRMWARE_ROOT)" -a -n "$(TEST_PACKAGE_ZIP)" || { echo 'Set FIRMWARE_ROOT and TEST_PACKAGE_ZIP'; exit 1; }
	$(PYTHON) tests/run_offline.py "$(FIRMWARE_ROOT)" "$(TEST_PACKAGE_ZIP)" $(if $(LEGACY_PACKAGE_ZIP),--legacy-package "$(LEGACY_PACKAGE_ZIP)" $(if $(filter 1 yes true,$(LEGACY_OS3_CONFIRMED)),--legacy-os3-confirmed)) --image "$(ARM_IMAGE)" $(OFFLINE_ARGS)

syntax:
	$(PYTHON) -m py_compile package.py tools/*.py tests/*.py
	@for script in *.sh; do sh -n "$$script" || exit; done
